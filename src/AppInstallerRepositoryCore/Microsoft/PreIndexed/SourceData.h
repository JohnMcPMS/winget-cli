// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once

#include <string>
#include <string_view>

namespace AppInstaller::Repository::Microsoft::PreIndexed
{
    // The identities of the packages that make up a pre-indexed source, as carried by
    // SourceDetails::Data.
    //
    // Data held exactly one package family name for as long as a source had exactly one package.
    // A delta capable source has two: the delta is published under its own identity, while the
    // baseline keeps the identity that the full index has always used. Both have to be known
    // before any network access, because the delta is the package that names its baseline and so
    // must be acquired first.
    //
    // Two syntaxes are therefore accepted:
    //
    //   Microsoft.Winget.Source_8wekyb3d8bbwe
    //   {"baseIdentity":"Microsoft.Winget.Source_8wekyb3d8bbwe","deltaIdentity":"..."}
    //
    // The first is what every client written to date has stored, and remains what is written back
    // for a source with no delta. The two are told apart by the leading brace, which a package
    // family name cannot contain.
    //
    // There is deliberately no migration from the first to the second. A source whose Data names
    // only a base identity uses the full index, which is exactly what it did before deltas
    // existed. A third party source that begins publishing a delta can be re-added, or can be
    // migrated by a later change.
    struct SourceData
    {
        SourceData() = default;

        // Parses a SourceDetails::Data value in either syntax.
        // Throws if the value is structured but malformed; an unparsable value is not silently
        // treated as a package family name.
        explicit SourceData(std::string_view data);

        // The identity the source has always been known by: the full index, and the baseline of a
        // delta pair.
        const std::string& BaseIdentity() const { return m_baseIdentity; }
        void BaseIdentity(std::string value) { m_baseIdentity = std::move(value); }

        // The identity of the delta, when the source publishes one.
        const std::string& DeltaIdentity() const { return m_deltaIdentity; }
        void DeltaIdentity(std::string value) { m_deltaIdentity = std::move(value); }

        bool HasBaseIdentity() const { return !m_baseIdentity.empty(); }
        bool HasDeltaIdentity() const { return !m_deltaIdentity.empty(); }

        // Produces the value to store back into SourceDetails::Data.
        //
        // A source with no delta serializes back to the bare family name it was read from, so
        // that the stored value does not churn for the sources that are not delta capable.
        std::string Serialize() const;

    private:
        std::string m_baseIdentity;
        std::string m_deltaIdentity;
    };
}
