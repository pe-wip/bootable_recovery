/*
 * Copyright (C) 2009 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "updater/updater.h"
#include "fsupdater/fsupdater.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <string>

#include <android-base/logging.h>
#include <android-base/strings.h>
#include <selinux/android.h>
#include <selinux/label.h>
#include <selinux/selinux.h>
#include <ziparchive/zip_archive.h>

#include "edify/expr.h"
#include "otautil/dirutil.h"
#include "otautil/error_code.h"
#include "otautil/sysutil.h"
#include "updater/blockimg.h"
#include "updater/dynamic_partitions.h"
#include "updater/install.h"

// Generated by the makefile, this function defines the
// RegisterDeviceExtensions() function, which calls all the
// registration functions for device-specific extensions.
#include "register.inc"

// Where in the package we expect to find the edify script to execute.
// (Note it's "updateR-script", not the older "update-script".)
static constexpr const char* SCRIPT_NAME = "META-INF/com/google/android/updater-script";

struct selabel_handle *sehandle;

static void UpdaterLogger(android::base::LogId /* id */, android::base::LogSeverity /* severity */,
                          const char* /* tag */, const char* /* file */, unsigned int /* line */,
                          const char* message) {
  fprintf(stdout, "%s\n", message);
}

int main(int argc, char** argv) {
  // Various things log information to stdout or stderr more or less
  // at random (though we've tried to standardize on stdout).  The
  // log file makes more sense if buffering is turned off so things
  // appear in the right order.
  setbuf(stdout, nullptr);
  setbuf(stderr, nullptr);

  // We don't have logcat yet under recovery. Update logs will always be written to stdout
  // (which is redirected to recovery.log).
  android::base::InitLogging(argv, &UpdaterLogger);

  if (argc != 4 && argc != 5) {
    LOG(ERROR) << "unexpected number of arguments: " << argc;
    return 1;
  }

  char* version = argv[1];
  if ((version[0] != '1' && version[0] != '2' && version[0] != '3') || version[1] != '\0') {
    // We support version 1, 2, or 3.
    LOG(ERROR) << "wrong updater binary API; expected 1, 2, or 3; got " << argv[1];
    return 2;
  }

  // Set up the pipe for sending commands back to the parent process.

  int fd = atoi(argv[2]);
  FILE* cmd_pipe = fdopen(fd, "wb");
  setlinebuf(cmd_pipe);

  // Extract the script from the package.

  const char* package_filename = argv[3];
  MemMapping map;
  if (!map.MapFile(package_filename)) {
    LOG(ERROR) << "failed to map package " << argv[3];
    return 3;
  }
  ZipArchiveHandle za;
  int open_err = OpenArchiveFromMemory(map.addr, map.length, argv[3], &za);
  if (open_err != 0) {
    LOG(ERROR) << "failed to open package " << argv[3] << ": " << ErrorCodeString(open_err);
    CloseArchive(za);
    return 3;
  }

  ZipString script_name(SCRIPT_NAME);
  ZipEntry script_entry;
  int find_err = FindEntry(za, script_name, &script_entry);
  if (find_err != 0) {
    LOG(ERROR) << "failed to find " << SCRIPT_NAME << " in " << package_filename << ": "
               << ErrorCodeString(find_err);
    CloseArchive(za);
    return 4;
  }

  std::string script;
  script.resize(script_entry.uncompressed_length);
  int extract_err = ExtractToMemory(za, &script_entry, reinterpret_cast<uint8_t*>(&script[0]),
                                    script_entry.uncompressed_length);
  if (extract_err != 0) {
    LOG(ERROR) << "failed to read script from package: " << ErrorCodeString(extract_err);
    CloseArchive(za);
    return 5;
  }

  // Configure edify's functions.

  RegisterBuiltins();
  RegisterFsUpdaterFunctions();
  RegisterInstallFunctions();
  RegisterBlockImageFunctions();
  RegisterDynamicPartitionsFunctions();
  RegisterDeviceExtensions();

  // Parse the script.

  std::unique_ptr<Expr> root;
  int error_count = 0;
  int error = ParseString(script, &root, &error_count);
  if (error != 0 || error_count > 0) {
    LOG(ERROR) << error_count << " parse errors";
    CloseArchive(za);
    return 6;
  }

  sehandle = selinux_android_file_context_handle();
  selinux_android_set_sehandle(sehandle);

  if (!sehandle) {
    fprintf(cmd_pipe, "ui_print Warning: No file_contexts\n");
  }

  // Evaluate the parsed script.

  UpdaterInfo updater_info;
  updater_info.cmd_pipe = cmd_pipe;
  updater_info.package_zip = za;
  updater_info.version = atoi(version);
  updater_info.package_zip_addr = map.addr;
  updater_info.package_zip_len = map.length;

  State state(script, &updater_info);

  if (argc == 5) {
    if (strcmp(argv[4], "retry") == 0) {
      state.is_retry = true;
    } else {
      printf("unexpected argument: %s", argv[4]);
    }
  }

  std::string result;
  bool status = Evaluate(&state, root, &result);

  if (!status) {
    if (state.errmsg.empty()) {
      LOG(ERROR) << "script aborted (no error message)";
      fprintf(cmd_pipe, "ui_print script aborted (no error message)\n");
    } else {
      LOG(ERROR) << "script aborted: " << state.errmsg;
      const std::vector<std::string> lines = android::base::Split(state.errmsg, "\n");
      for (const std::string& line : lines) {
        // Parse the error code in abort message.
        // Example: "E30: This package is for bullhead devices."
        if (!line.empty() && line[0] == 'E') {
          if (sscanf(line.c_str(), "E%d: ", &state.error_code) != 1) {
            LOG(ERROR) << "Failed to parse error code: [" << line << "]";
          }
        }
        fprintf(cmd_pipe, "ui_print %s\n", line.c_str());
      }
    }

    // Installation has been aborted. Set the error code to kScriptExecutionFailure unless
    // a more specific code has been set in errmsg.
    if (state.error_code == kNoError) {
      state.error_code = kScriptExecutionFailure;
    }
    fprintf(cmd_pipe, "log error: %d\n", state.error_code);
    // Cause code should provide additional information about the abort.
    if (state.cause_code != kNoCause) {
      fprintf(cmd_pipe, "log cause: %d\n", state.cause_code);
      if (state.cause_code == kPatchApplicationFailure) {
        LOG(INFO) << "Patch application failed, retry update.";
        fprintf(cmd_pipe, "retry_update\n");
      } else if (state.cause_code == kEioFailure) {
        LOG(INFO) << "Update failed due to EIO, retry update.";
        fprintf(cmd_pipe, "retry_update\n");
      }
    }

    if (updater_info.package_zip) {
      CloseArchive(updater_info.package_zip);
    }
    return 7;
  } else {
    fprintf(cmd_pipe, "ui_print script succeeded: result was [%s]\n", result.c_str());
  }

  if (updater_info.package_zip) {
    CloseArchive(updater_info.package_zip);
  }

  return 0;
}
