// Copyright (c) 2013 GitHub, Inc.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#include "atom/common/crash_reporter/crash_reporter_win.h"

#include <memory>
#include <vector>

#include "atom/browser/api/atom_api_web_contents.h"
#include "atom/common/atom_constants.h"
#include "base/environment.h"
#include "base/memory/singleton.h"
#include "base/path_service.h"
#include "base/strings/stringprintf.h"
#include "base/strings/utf_string_conversions.h"
#include "electron/atom/common/api/api.mojom.h"
#include "third_party/blink/public/common/associated_interfaces/associated_interface_provider.h"
#include "third_party/crashpad/crashpad/client/crashpad_client.h"
#include "third_party/crashpad/crashpad/client/crashpad_info.h"

#if defined(_WIN64)
#include "gin/public/debug.h"
#endif

namespace {

#if defined(_WIN64)
int CrashForException(EXCEPTION_POINTERS* info) {
  auto* reporter = crash_reporter::CrashReporterWin::GetInstance();
  if (reporter->IsInitialized())
    reporter->GetCrashpadClient().DumpAndCrash(info);
  return EXCEPTION_CONTINUE_SEARCH;
}
#endif

}  // namespace

namespace crash_reporter {

CrashReporterWin::CrashReporterWin() {}

CrashReporterWin::~CrashReporterWin() {}

#if defined(_WIN64)
void CrashReporterWin::SetUnhandledExceptionFilter() {
  gin::Debug::SetUnhandledExceptionCallback(&CrashForException);
}
#endif

void CrashReporterWin::Init(const std::string& product_name,
                            const std::string& version,
                            const std::string& company_name,
                            const std::string& submit_url,
                            const base::FilePath& crashes_dir,
                            bool upload_to_server,
                            bool skip_system_crash_handler) {
  // check whether crashpad has been initialized.
  // Only need to initialize once.
  if (simple_string_dictionary_)
    return;
  if (process_type_.empty()) {  // browser process
    base::FilePath handler_path;
    base::PathService::Get(base::FILE_EXE, &handler_path);

    std::vector<std::string> args = {
        "--no-rate-limit",
        "--no-upload-gzip",  // not all servers accept gzip
    };
    args.push_back(base::StringPrintf("--type=%s", kCrashpadProcess));
    args.push_back(
        base::StringPrintf("--%s=%s", kCrashesDirectoryKey,
                           base::UTF16ToUTF8(crashes_dir.value()).c_str()));
    crashpad_client_.StartHandler(handler_path, crashes_dir, crashes_dir,
                                  submit_url, StringMap(), args, true, false);
    UpdatePipeName();
  } else {
    std::unique_ptr<base::Environment> env(base::Environment::Create());
    std::string pipe_name_utf8;
    if (env->GetVar(atom::kCrashpadPipeName, &pipe_name_utf8)) {
      base::string16 pipe_name = base::UTF8ToUTF16(pipe_name_utf8);
      if (!crashpad_client_.SetHandlerIPCPipe(pipe_name))
        LOG(ERROR) << "Failed to set handler IPC pipe name: " << pipe_name;
    } else {
      LOG(ERROR) << "Unable to get pipe name for crashpad";
    }
  }
  crashpad::CrashpadInfo* crashpad_info =
      crashpad::CrashpadInfo::GetCrashpadInfo();
  if (skip_system_crash_handler) {
    crashpad_info->set_system_crash_reporter_forwarding(
        crashpad::TriState::kDisabled);
  }
  simple_string_dictionary_.reset(new crashpad::SimpleStringDictionary());
  crashpad_info->set_simple_annotations(simple_string_dictionary_.get());

  SetInitialCrashKeyValues(version);
  if (process_type_.empty()) {  // browser process
    database_ = crashpad::CrashReportDatabase::Initialize(crashes_dir);
    SetUploadToServer(upload_to_server);
  }
}

void CrashReporterWin::SetUploadParameters() {
  upload_parameters_["platform"] = "win32";
}

crashpad::CrashpadClient& CrashReporterWin::GetCrashpadClient() {
  return crashpad_client_;
}

void CrashReporterWin::UpdatePipeName() {
  std::string pipe_name =
      base::UTF16ToUTF8(crashpad_client_.GetHandlerIPCPipe());
  std::unique_ptr<base::Environment> env(base::Environment::Create());
  env->SetVar(atom::kCrashpadPipeName, pipe_name);

  // Notify all WebContents of the pipe name.
  v8::Isolate* isolate = v8::Isolate::GetCurrent();
  auto pages = atom::api::WebContents::GetAll(isolate);
  for (const auto& value : pages) {
    mate::Handle<atom::api::WebContents> web_contents;
    if (!mate::ConvertFromV8(isolate, value, &web_contents))
      continue;
    if (web_contents->GetType() == atom::api::WebContents::Type::REMOTE)
      continue;
    auto* frame_host = web_contents->web_contents()->GetMainFrame();
    if (!frame_host)
      continue;

    atom::mojom::ElectronRendererAssociatedPtr electron_ptr;
    frame_host->GetRemoteAssociatedInterfaces()->GetInterface(
        mojo::MakeRequest(&electron_ptr));
    electron_ptr->UpdateCrashpadPipeName(pipe_name);
  }
}

// static
CrashReporterWin* CrashReporterWin::GetInstance() {
  return base::Singleton<CrashReporterWin>::get();
}

// static
CrashReporter* CrashReporter::GetInstance() {
  return CrashReporterWin::GetInstance();
}

}  // namespace crash_reporter
