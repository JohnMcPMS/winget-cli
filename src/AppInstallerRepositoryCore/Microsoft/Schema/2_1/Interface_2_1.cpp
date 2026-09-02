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

        SQLite::Savepoint savepoint = SQLite::Savepoint::Create(connection, "migrate_from_v2_1");

        // Attempt a migration to 2.0 first, which will only return true if it actually performed a migration
        bool v2result = V2_0::Interface::MigrateFrom(connection, current);

        // Migration from 2.0 → 2.1: add the is_removed column to update_tracking.
        if (v2result || (currentVersion.MajorVersion == 2 && currentVersion.MinorVersion == 0))
        {
            V2_0::PackageUpdateTrackingTable::AddIsRemovedColumn(connection);
            savepoint.Commit();
            return true;
        }

        savepoint.Rollback(true);
        return false;
    }

    V2_0::PackageUpdateTrackingTable::RemovalBehavior Interface::GetTrackingRemovalBehavior() const
    {
        return V2_0::PackageUpdateTrackingTable::RemovalBehavior::Record;
    }
}
