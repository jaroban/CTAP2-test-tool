// Copyright 2020 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "src/device_tracker.h"

#include <filesystem>
#include <fstream>
#include <iostream>

#include "absl/time/clock.h"
#include "src/parameter_check.h"
#include "third_party/chromium_components_cbor/values.h"

extern const char build_scm_revision[];

namespace fido2_tests {
namespace {
constexpr std::string_view kRelativeDir = "results/";
constexpr std::string_view kFileType = ".json";

// Creates a directory for results files and returns the path. Just return
// the path if that directory already exists. Fails if the directory wasn't
// created successfully.
std::string CreateSaveFileDirectory() {
  std::string results_dir = std::string(kRelativeDir);
  if (const char* env_dir = std::getenv("BUILD_WORKSPACE_DIRECTORY")) {
    results_dir = absl::StrCat(env_dir, "/", results_dir);
  }
  std::filesystem::create_directory(results_dir);
  return results_dir;
}

void PrintSuccessMessage(std::string_view message) {
  std::cout << "\x1b[0;32m" << message << "\x1b[0m" << std::endl;
}

void PrintWarningMessage(std::string_view message) {
  std::cout << "\x1b[0;33m" << message << "\x1b[0m" << std::endl;
}

void PrintFailMessage(std::string_view message) {
  std::cout << "\x1b[0;31m" << message << "\x1b[0m" << std::endl;
}

}  // namespace

DeviceTracker::DeviceTracker()
    : key_checker_(std::vector<std::vector<uint8_t>>()) {}

void DeviceTracker::Initialize(const cbor::Value::ArrayValue& versions,
                               const cbor::Value::ArrayValue& extensions,
                               const cbor::Value::MapValue& options) {
  if (is_initialized_) {
    return;
  }
  is_initialized_ = true;

  for (const auto& versions_iter : versions) {
    if (versions_iter.is_string()) {
      versions_.insert(versions_iter.GetString());
    }
  }

  for (const auto& extensions_iter : extensions) {
    if (extensions_iter.is_string()) {
      extensions_.insert(extensions_iter.GetString());
    }
  }

  absl::flat_hash_set<std::string> mutable_options = {"clientPin", "uv",
                                                      "bioEnroll"};
  for (const auto& options_iter : options) {
    if (options_iter.first.is_string() && options_iter.second.is_bool()) {
      bool is_mutable =
          mutable_options.contains(options_iter.first.GetString());
      if (is_mutable || options_iter.second.GetBool()) {
        options_.insert(options_iter.first.GetString());
      }
    }
  }
}

bool DeviceTracker::HasVersion(std::string_view version_name) const {
  return versions_.contains(version_name);
}

bool DeviceTracker::HasExtension(std::string_view extension_name) const {
  return extensions_.contains(extension_name);
}

bool DeviceTracker::HasOption(std::string_view option_name) const {
  return options_.contains(option_name);
}

bool DeviceTracker::HasWinkCapability() const { return has_wink_capability_; }

bool DeviceTracker::HasCborCapability() const { return has_cbor_capability_; }

void DeviceTracker::SetCapabilities(bool wink, bool cbor, bool msg) {
  has_wink_capability_ = wink;
  has_cbor_capability_ = cbor;
  has_msg_capability_ = msg;
}

void DeviceTracker::SetDeviceIdentifiers(DeviceIdentifiers device_identifiers) {
  device_identifiers_ = std::move(device_identifiers);
}

void DeviceTracker::SetAaguid(std::string_view aaguid) { aaguid_ = aaguid; }

void DeviceTracker::IgnoreNextTouchPrompt() { ignores_touch_prompt_ = true; }

bool DeviceTracker::IsTouchPromptIgnored() {
  bool reponse = ignores_touch_prompt_;
  ignores_touch_prompt_ = false;
  return reponse;
}

void DeviceTracker::AddObservation(const std::string& observation) {
  if (std::find(observations_.begin(), observations_.end(), observation) ==
      observations_.end()) {
    std::cout << observation << std::endl;
    observations_.push_back(observation);
  }
}

void DeviceTracker::AddProblem(const std::string& problem) {
  PrintWarningMessage(problem);
  if (std::find(problems_.begin(), problems_.end(), problem) ==
      problems_.end()) {
    problems_.push_back(problem);
  }
}

void DeviceTracker::AssertCondition(bool condition, std::string_view message) {
  if (!condition) {
    ReportFindings();
    SaveResultsToFile();
  }
  CHECK(condition) << "Failed critical condition: " << message;
}

void DeviceTracker::AssertStatus(Status status, std::string_view message) {
  AssertCondition(CheckStatus(status),
                  absl::StrCat(message, " - returned status code ",
                               StatusToString(status)));
}

void DeviceTracker::AssertResponse(
    const absl::variant<cbor::Value, Status>& returned_variant,
    std::string_view message) {
  AssertCondition(CheckStatus(returned_variant), message);
}

bool DeviceTracker::CheckStatus(Status status) {
  bool ok = status == Status::kErrNone;
  if (!ok) {
    AddObservation(absl::StrCat("The failing error code is `",
                                StatusToString(status), "`."));
  }
  return ok;
}

bool DeviceTracker::CheckStatus(Status expected_status,
                                Status returned_status) {
  if (expected_status == Status::kErrNone) {
    // Use the one argument function to print the correct observation.
    return CheckStatus(returned_status);
  }
  if (expected_status != returned_status) {
    AddObservation(absl::StrCat("Expected error code `",
                                StatusToString(expected_status), "`, got `",
                                StatusToString(returned_status), "`."));
  }
  return returned_status != Status::kErrNone;
}

bool DeviceTracker::CheckStatus(
    const absl::variant<cbor::Value, Status>& returned_variant) {
  Status returned_status = Status::kErrNone;
  if (absl::holds_alternative<Status>(returned_variant)) {
    returned_status = absl::get<Status>(returned_variant);
  }
  return CheckStatus(returned_status);
}

void DeviceTracker::LogTest(std::string test_id, std::string test_description,
                            std::optional<std::string> error_message) {
  tests_.push_back(
      TestResult{std::move(test_id), test_description, error_message});
  // Backwards compatibility by using the old interface.
  if (error_message.has_value()) {
    std::string fail_message =
        absl::StrCat(test_description, " - ", error_message.value());
    PrintFailMessage(absl::StrCat("Failed test: ", fail_message));
    failed_tests_.push_back(fail_message);
  } else {
    PrintSuccessMessage(absl::StrCat("Test successful: ", test_description));
    successful_tests_.push_back(test_description);
  }
}

KeyChecker* DeviceTracker::GetKeyChecker() { return &key_checker_; }

CounterChecker* DeviceTracker::GetCounterChecker() { return &counter_checker_; }

void DeviceTracker::ReportFindings() const {
  std::cout << counter_checker_.ReportFindings() << "\n\n";
  for (std::string_view observation : observations_) {
    std::cout << observation << "\n";
  }
  std::cout << std::endl;
  for (std::string_view problem : problems_) {
    PrintWarningMessage(problem);
  }
  std::cout << std::endl;
  for (std::string_view test : failed_tests_) {
    PrintFailMessage(test);
  }
  int successful_test_count = successful_tests_.size();
  int failed_test_count = failed_tests_.size();
  int test_count = successful_test_count + failed_test_count;
  std::cout << "Passed " << successful_test_count << " out of " << test_count
            << " tests." << std::endl;
}

nlohmann::json DeviceTracker::GenerateResultsJson(
    std::string_view commit_hash, std::string_view time_string) const {
  int successful_test_count = successful_tests_.size();
  int failed_test_count = failed_tests_.size();
  int test_count = successful_test_count + failed_test_count;

  nlohmann::json results = {
      {"passed_test_count", successful_test_count},
      {"total_test_count", test_count},
      {"failed_tests", failed_tests_},
      {"problems", problems_},
      {"observations", observations_},
      {"date", time_string},
      {"commit", commit_hash},
      {
          "device_under_test",
          {
              {"manufacturer", device_identifiers_.manufacturer},
              {"product_name", device_identifiers_.product_name},
              {"serial_number", device_identifiers_.serial_number},
              {"vendor_id",
               absl::StrCat("0x", absl::Hex(device_identifiers_.vendor_id,
                                            absl::kZeroPad4))},
              {"product_id",
               absl::StrCat("0x", absl::Hex(device_identifiers_.product_id,
                                            absl::kZeroPad4))},
              {"aaguid", aaguid_},
              {"url", nullptr},
          },
      },
      {
          "capabilities",
          {
              {"versions",
               std::vector<std::string>(versions_.begin(), versions_.end())},
              {"options",
               std::vector<std::string>(options_.begin(), options_.end())},
              {"extensions", std::vector<std::string>(extensions_.begin(),
                                                      extensions_.end())},
              {"wink", has_wink_capability_},
              {"cbor", has_cbor_capability_},
              {"msg", has_msg_capability_},
              {"signature_counter", counter_checker_.ReportFindings()},
          },
      },
  };
  return results;
}

void DeviceTracker::SaveResultsToFile() const {
  absl::Time now = absl::Now();
  absl::TimeZone local = absl::LocalTimeZone();
  std::string time_string = absl::FormatTime("%Y-%m-%d", now, local);

  std::filesystem::path results_path =
      absl::StrCat(CreateSaveFileDirectory(), device_identifiers_.product_name,
                   "_", device_identifiers_.serial_number, kFileType);
  std::ofstream results_file;
  results_file.open(results_path);
  CHECK(results_file.is_open()) << "Unable to open file: " << results_path;

  results_file << std::setw(2)
               << GenerateResultsJson(build_scm_revision, time_string)
               << std::endl;
}

}  // namespace fido2_tests

