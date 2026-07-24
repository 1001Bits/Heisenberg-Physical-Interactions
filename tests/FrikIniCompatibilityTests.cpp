#include "FrikIniCompatibility.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace
{
    using heisenberg::frik_ini_compat::DisableOffHandGripping;
    using heisenberg::frik_ini_compat::RewriteResult;

    void Require(const bool condition, const char* message)
    {
        if (!condition) {
            std::cerr << "FAILED: " << message << '\n';
            std::exit(1);
        }
    }
}

int main()
{
    {
        std::string ini = "; comment\r\n[Fallout4VRBody]\r\nEnableOffHandGripping = true\r\nGripButtonID = 2\r\n";
        const auto result = DisableOffHandGripping(ini);
        Require(result == RewriteResult::UpdatedExistingKey, "true setting should be updated");
        Require(ini == "; comment\r\n[Fallout4VRBody]\r\nEnableOffHandGripping = false\r\nGripButtonID = 2\r\n",
            "rewrite should preserve CRLF and unrelated bytes");
    }

    {
        std::string ini = "[Fallout4VRBody]\n  enableoffhandgripping = OFF ; retained comment\n";
        const std::string before = ini;
        const auto result = DisableOffHandGripping(ini);
        Require(result == RewriteResult::AlreadyDisabled, "false-equivalent setting should be unchanged");
        Require(ini == before, "already-disabled input must remain byte-identical");
    }

    {
        std::string ini = "[Fallout4VRBody]\nGripButtonID = 2\n[Other]\nValue = 1\n";
        const auto result = DisableOffHandGripping(ini);
        Require(result == RewriteResult::AddedMissingKey, "missing key should be added because FRIK defaults it true");
        Require(ini.find("EnableOffHandGripping = false\n[Other]") != std::string::npos,
            "missing key should be inserted into the FRIK section");
    }

    {
        std::string ini = "\xEF\xBB\xBF[Other]\r\nValue = 1\r\n";
        const auto result = DisableOffHandGripping(ini);
        Require(result == RewriteResult::AddedMissingSection, "missing FRIK section should be added");
        Require(ini.ends_with("[Fallout4VRBody]\r\nEnableOffHandGripping = false\r\n"),
            "new section should retain the document newline style");
    }

    {
        std::string ini = "[Fallout4VRBody]\nEnableOffHandGripping = yes\nEnableOffHandGripping=1\n";
        const auto result = DisableOffHandGripping(ini);
        Require(result == RewriteResult::UpdatedExistingKey, "all duplicate enabled keys should be disabled");
        Require(ini == "[Fallout4VRBody]\nEnableOffHandGripping = false\nEnableOffHandGripping=false\n",
            "duplicate settings should all be rewritten");
    }

    return 0;
}
