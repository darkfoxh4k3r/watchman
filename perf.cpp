/* Copyright 2016-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"
#include "watchman_perf.h"
#include "watchman_synchronized.h"
#include <condition_variable>
#include <thread>

namespace {
class PerfLogThread {
  watchman::Synchronized<json_ref, std::mutex> samples_;
  std::thread thread_;
  std::condition_variable cond_;

  void loop();

 public:
  PerfLogThread() : thread_([this]() { loop(); }) {}

  ~PerfLogThread() {
    cond_.notify_all();
    thread_.join();
  }

  void addSample(json_ref&& sample) {
    auto wlock = samples_.wlock();
    if (!*wlock) {
      *wlock = json_array();
    }
    json_array_append_new(*wlock, std::move(sample));
    cond_.notify_one();
  }
};

watchman::Synchronized<std::unique_ptr<PerfLogThread>> perfThread;
}

watchman_perf_sample::watchman_perf_sample(const char* description)
    : description(description) {
  gettimeofday(&time_begin, nullptr);
#ifdef HAVE_SYS_RESOURCE_H
  getrusage(RUSAGE_SELF, &usage_begin);
#endif
}

bool watchman_perf_sample::finish() {
  gettimeofday(&time_end, nullptr);
  w_timeval_sub(time_end, time_begin, &duration);
#ifdef HAVE_SYS_RESOURCE_H
  getrusage(RUSAGE_SELF, &usage_end);

  // Compute the delta for the usage
  w_timeval_sub(usage_end.ru_utime, usage_begin.ru_utime, &usage.ru_utime);
  w_timeval_sub(usage_end.ru_stime, usage_begin.ru_stime, &usage.ru_stime);

#define DIFFU(n) usage.n = usage_end.n - usage_begin.n
  DIFFU(ru_maxrss);
  DIFFU(ru_ixrss);
  DIFFU(ru_idrss);
  DIFFU(ru_minflt);
  DIFFU(ru_majflt);
  DIFFU(ru_nswap);
  DIFFU(ru_inblock);
  DIFFU(ru_oublock);
  DIFFU(ru_msgsnd);
  DIFFU(ru_msgrcv);
  DIFFU(ru_nsignals);
  DIFFU(ru_nvcsw);
  DIFFU(ru_nivcsw);
#undef DIFFU
#endif

  if (!will_log) {
    if (wall_time_elapsed_thresh == 0) {
      auto thresh = cfg_get_json("perf_sampling_thresh");
      if (thresh) {
        if (json_is_number(thresh)) {
          wall_time_elapsed_thresh = json_number_value(thresh);
        } else {
          json_unpack(thresh, "{s:f}", description, &wall_time_elapsed_thresh);
        }
      }
    }

    if (wall_time_elapsed_thresh > 0 &&
        w_timeval_diff(time_begin, time_end) > wall_time_elapsed_thresh) {
      will_log = true;
    }
  }

  return will_log;
}

void watchman_perf_sample::add_meta(const char* key, json_ref&& val) {
  if (!meta_data) {
    meta_data = json_object();
  }
  meta_data.set(key, std::move(val));
}

void watchman_perf_sample::add_root_meta(
    const std::shared_ptr<w_root_t>& root) {
  // Note: if the root lock isn't held, we may read inaccurate numbers for
  // some of these properties.  We're ok with that, and don't want to force
  // the root lock to be re-acquired just for this.
  auto meta = json_object(
      {{"path", w_string_to_json(root->root_path)},
       {"recrawl_count", json_integer(root->recrawlInfo.rlock()->recrawlCount)},
       {"case_sensitive", json_boolean(root->case_sensitive)}});

  // During recrawl, the view may be re-assigned.  Protect against
  // reading a nullptr.
  auto view = root->inner.view;
  if (view) {
    auto position = view->getMostRecentRootNumberAndTickValue();
    meta.set({{"number", json_integer(position.rootNumber)},
              {"ticks", json_integer(position.ticks)},
              {"watcher", w_string_to_json(view->getName())}});
  }

  add_meta("root", std::move(meta));
}

void watchman_perf_sample::set_wall_time_thresh(double thresh) {
  wall_time_elapsed_thresh = thresh;
}

void watchman_perf_sample::force_log() {
  will_log = true;
}

void PerfLogThread::loop() {
  json_ref samples;
  char **envp;
  json_ref perf_cmd;
  int64_t sample_batch;

  w_set_thread_name("perflog");

  // Prep some things that we'll need each time we run a command
  {
    uint32_t env_size;
    auto envpht = w_envp_make_ht();
    char *statedir = dirname(strdup(watchman_state_file));
    w_envp_set_cstring(envpht, "WATCHMAN_STATE_DIR", statedir);
    w_envp_set_cstring(envpht, "WATCHMAN_SOCK", get_sock_name());
    envp = w_envp_make_from_ht(envpht, &env_size);
  }

  perf_cmd = cfg_get_json("perf_logger_command");
  if (json_is_string(perf_cmd)) {
    perf_cmd = json_array({perf_cmd});
  }
  if (!json_is_array(perf_cmd)) {
    w_log(
        W_LOG_FATAL,
        "perf_logger_command must be either a string or an array of strings\n");
  }

  sample_batch = cfg_get_int("perf_logger_command_max_samples_per_call", 4);

  while (!w_is_stopping()) {
    {
      auto wlock = samples_.wlock();
      if (!*wlock) {
        cond_.wait(wlock.getUniqueLock());
      }

      samples = nullptr;
      std::swap(samples, *wlock);
    }

    if (samples) {
      while (json_array_size(samples) > 0) {
        int i = 0;
        auto cmd = json_array();
        posix_spawnattr_t attr;
        posix_spawn_file_actions_t actions;
        pid_t pid;
        char **argv = NULL;

        json_array_extend(cmd, perf_cmd);

        while (i < sample_batch && json_array_size(samples) > 0) {
          char *stringy = json_dumps(json_array_get(samples, 0), 0);
          if (stringy) {
            json_array_append_new(
                cmd, typed_string_to_json(stringy, W_STRING_MIXED));
            free(stringy);
          }
          json_array_remove(samples, 0);
          i++;
        }

        argv = w_argv_copy_from_json(cmd, 0);
        if (!argv) {
          char *dumped = json_dumps(cmd, 0);
          w_log(W_LOG_FATAL, "error converting %s to an argv array\n", dumped);
        }

        posix_spawnattr_init(&attr);
#ifdef POSIX_SPAWN_CLOEXEC_DEFAULT
        posix_spawnattr_setflags(&attr, POSIX_SPAWN_CLOEXEC_DEFAULT);
#endif
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null",
                                         O_RDONLY, 0666);
        posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null",
                                         O_WRONLY, 0666);
        posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null",
                                         O_WRONLY, 0666);

        if (posix_spawnp(&pid, argv[0], &actions, &attr, argv, envp) == 0) {
          int status;
          while (waitpid(pid, &status, 0) != pid) {
            if (errno != EINTR) {
              break;
            }
          }
        } else {
          int err = errno;
          w_log(W_LOG_ERR, "failed to spawn %s: %s\n", argv[0],
                strerror(err));
        }

        posix_spawnattr_destroy(&attr);
        posix_spawn_file_actions_destroy(&actions);

        free(argv);
      }
    }
  }
}

void watchman_perf_sample::log() {
  char *dumped = NULL;

  if (!will_log) {
    return;
  }

  // Assemble a perf blob
  auto info = json_object(
      {{"description", typed_string_to_json(description)},
       {"meta", std::move(meta_data)},
       {"pid", json_integer(getpid())},
       {"version", typed_string_to_json(PACKAGE_VERSION, W_STRING_UNICODE)}});

#ifdef WATCHMAN_BUILD_INFO
  info.set(
      "buildinfo", typed_string_to_json(WATCHMAN_BUILD_INFO, W_STRING_UNICODE));
#endif

#define ADDTV(name, tv) info.set(name, json_real(w_timeval_abs_seconds(tv)))
  ADDTV("elapsed_time", duration);
  ADDTV("start_time", time_begin);
#ifdef HAVE_SYS_RESOURCE_H
  ADDTV("user_time", usage.ru_utime);
  ADDTV("system_time", usage.ru_stime);
#define ADDU(n) info.set(#n, json_integer(usage.n))
  ADDU(ru_maxrss);
  ADDU(ru_ixrss);
  ADDU(ru_idrss);
  ADDU(ru_minflt);
  ADDU(ru_majflt);
  ADDU(ru_nswap);
  ADDU(ru_inblock);
  ADDU(ru_oublock);
  ADDU(ru_msgsnd);
  ADDU(ru_msgrcv);
  ADDU(ru_nsignals);
  ADDU(ru_nvcsw);
  ADDU(ru_nivcsw);
#endif // HAVE_SYS_RESOURCE_H
#undef ADDU
#undef ADDTV

  // Log to the log file
  dumped = json_dumps(info, 0);
  w_log(W_LOG_ERR, "PERF: %s\n", dumped);
  free(dumped);

  if (!cfg_get_json("perf_logger_command")) {
    return;
  }

  // Send this to our logging thread for async processing

  {
    // The common case is that we already set up the logging
    // thread and that we can just log through it.
    auto rlock = perfThread.rlock();
    if (rlock->get()) {
      (*rlock)->addSample(std::move(info));
      return;
    }
  }

  // If it wasn't set, then we need an exclusive lock to
  // make sure that we don't spawn multiple instances.
  {
    auto wlock = perfThread.wlock();

    if (!wlock->get()) {
      *wlock = watchman::make_unique<PerfLogThread>();
    }

    (*wlock)->addSample(std::move(info));
  }
}

/* vim:ts=2:sw=2:et:
 */
