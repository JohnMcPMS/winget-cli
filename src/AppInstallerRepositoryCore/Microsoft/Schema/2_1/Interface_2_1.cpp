// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#include "pch.h"
#include "Interface.h"
#include "Microsoft/Schema/2_0/PackageUpdateTrackingTable.h"

namespace AppInstaller::Repository::Microsoft::Schema::V2_1
{
    Interface::Interface(Utility::NormalizationVersion normVersion) : V2_0::Interface(normVersion) {}

    SQLite::Version Interface::GetVersion() const
    {
        return { 2, 1 };
    }

    bool Interface::MigrateFrom(SQLite::Connection& connection, const ISQLiteIndex* current)
    {
        THROW_HR_IF_NULL(E_POINTER, current);

        auto currentVersion = current->GetVersion();

        // Migration from 2.0 → 2.1: add the is_removed column to update_tracking.
        if (currentVersion.MajorVersion == 2 && currentVersion.MinorVersion == 0)
        {
            SQLite::Savepoint savepoint = SQLite::Savepoint::Create(connection, "migrate_from_v2_1");
            V2_0::PackageUpdateTrackingTable::EnsureIsRemovedColumn(connection);
            savepoint.Commit();
            return true;
        }

        // Fall through to V2_0 migration (handles 1.7 → 2.0 → 2.1 via two-step upgrade).
        return V2_0::Interface::MigrateFrom(connection, current);
    }
}
