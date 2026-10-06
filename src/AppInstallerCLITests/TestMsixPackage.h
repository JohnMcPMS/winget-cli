// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once
#include <filesystem>
#include <string>

namespace TestCommon
{
    // Describes a source index package to build.
    //
    // The defaults match the signed test data packages, so a test that only cares about having
    // *some* package can leave them alone and still produce something the existing fixtures
    // recognize.
    struct TestIndexPackageDefinition
    {
        // The identity name. Together with the publisher this determines the package family name.
        std::string Name = "AppInstallerCLITestsFakeIndex";

        // Must be a well formed distinguished name; the publisher hash is computed from it.
        std::string Publisher = "CN=Code Sign Test (DO NOT TRUST), O=Microsoft Corporation, L=Redmond, S=Washington, C=US";

        // Four part version. This is what the source update path compares to decide whether it
        // already holds a package, so it is the field most tests will vary.
        std::string Version = "1.0.0.0";
    };

    // Builds an unsigned MSIX at outputPath carrying indexFile as `Public\index.db`, which is where
    // the pre-indexed source expects to find an index.
    //
    // The result will not pass trust validation; drive it through
    // TestHook::SetSourcePackageTrustValidation_Override. Building rather than checking in a signed
    // package is what lets a test choose the identities and versions that the delta pairing logic
    // turns on, and it removes the certificate installation that forces the existing `[pips]` cases
    // to require administrator rights.
    //
    // Returns the package family name of the result, read back out of the package that was written
    // rather than computed, so that a test asserting on identity is asserting on what it built.
    std::string CreateTestIndexPackage(
        const std::filesystem::path& outputPath,
        const std::filesystem::path& indexFile,
        const TestIndexPackageDefinition& definition = {});
}
