// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once
#include "Microsoft/Schema/2_0/Interface.h"

namespace AppInstaller::Repository::Microsoft::Schema::V2_1
{
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
    };
}
