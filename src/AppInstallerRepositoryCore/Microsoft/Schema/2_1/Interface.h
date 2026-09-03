// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once
#include "Microsoft/Schema/2_0/Interface.h"

namespace AppInstaller::Repository::Microsoft::Schema::V2_1
{
    // The point in time from which the next delta generated against this index should be computed.
    static constexpr std::string_view s_MetadataValueName_DeltaBaselineTime = "deltaBaselineTime"sv;

    // Interface to schema version 2.1 exposed through ISQLiteIndex.
    // Version 2.1 adds the is_removed column to the update_tracking table,
    // enabling delta index generation that can represent package removals.
    struct Interface : public V2_0::Interface
    {
        Interface(Utility::NormalizationVersion normVersion = Utility::NormalizationVersion::Initial);

        // Version 1.0
        SQLite::Version GetVersion() const override;

        // Version 2.0
        bool MigrateFrom(SQLite::Connection& connection, const ISQLiteIndex* current) override;

        // Sets this index up to read the combination of a delta and the baseline it was generated
        // against. Attaches the baseline and defines the merged views, after which every inherited
        // read path operates on the combination. Must be called before any read.
        void SetupDeltaReadMode(SQLite::Connection& connection, const std::string& baselinePath);

    protected:
        // Records the baseline time for this index, and generates a delta index against a previous
        // baseline when the caller has supplied the paths to do so.
        void CreateAdditionalPackagingOutput(const SQLiteIndexContext& context) override;
    };
}
