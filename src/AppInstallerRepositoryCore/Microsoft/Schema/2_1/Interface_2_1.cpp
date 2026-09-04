// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#include "pch.h"
#include "Interface.h"
#include "Microsoft/Schema/2_0/PackageUpdateTrackingTable.h"
#include "Microsoft/Schema/2_1/DeltaGeneration.h"
#include "Microsoft/Schema/2_1/DeltaViews.h"

#include <winget/SQLiteMetadataTable.h>
#include <AppInstallerDateTime.h>

#include <sstream>

namespace AppInstaller::Repository::Microsoft::Schema::V2_1
{
    Interface::Interface(Utility::NormalizationVersion normVersion) : V2_0::Interface(normVersion)
    {
        // Removals are recorded rather than deleted, so that delta generation can see which
        // packages have gone away. This is the difference that 2.1 exists for.
        m_trackingRemovalBehavior = V2_0::PackageUpdateTrackingTable::RemovalBehavior::Record;
    }

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
            V2_0::PackageUpdateTrackingTable::AddRemovalTrackingColumns(connection);
            savepoint.Commit();
            return true;
        }

        savepoint.Rollback(true);
        return false;
    }

    void Interface::MarkAsBaseline(SQLite::Connection& connection)
    {
        GUID baselineIdentifier;
        THROW_IF_FAILED(CoCreateGuid(&baselineIdentifier));

        std::ostringstream stream;
        stream << baselineIdentifier;
        std::string value = stream.str();

        AICLI_LOG(Repo, Info, << "Marking index as a delta baseline with identifier [" << value << "]");

        SQLite::MetadataTable::SetNamedValue(connection, s_MetadataValueName_BaselineIdentifier, value);
    }

    void Interface::SetupDeltaReadMode(SQLite::Connection& connection, const SQLite::DatabaseSpecifier& baseline)
    {
        Delta::SetupReadMode(connection, baseline);

        // The merged data is presented through views rather than tables, so the checks that the
        // base makes to decide whether this index has been packaged cannot see it. Record that the
        // question is already settled: a delta is only ever read, and only in its packaged form.
        m_isDeltaReadMode = true;
        m_internalInterfaceChecked = true;
    }

    void Interface::CreateAdditionalPackagingOutput(const SQLiteIndexContext& context)
    {
        SQLite::Connection& connection = context.Connection;

        // Record the point from which a delta against this index should be computed. Every 2.1 index
        // does this, because any of them may later be designated as a baseline.
        // TODO: We may need to set the baseline time to the max update tracking time +1 to only catch new incoming changes
        //       This assumes some delay between delta generation and the next package update.
        // TODO: We also need to ensure that our times are UTC / not impacted by timezone shifts, etc.
        SQLite::MetadataTable::SetNamedValue(connection, s_MetadataValueName_DeltaBaselineTime, std::to_string(Utility::GetCurrentUnixEpoch()));

        if (!context.Data.Contains(Property::DeltaBaselineIndexPath) ||
            !context.Data.Contains(Property::DeltaOutputPath))
        {
            return;
        }

        std::filesystem::path baselinePath = context.Data.Get<Property::DeltaBaselineIndexPath>();
        std::filesystem::path deltaOutputPath = context.Data.Get<Property::DeltaOutputPath>();

        AICLI_LOG(Repo, Info, << "Generating a delta index against baseline [" << baselinePath << "]");

        SQLite::Connection baselineConnection = SQLite::Connection::Create(baselinePath.u8string(), SQLite::Connection::OpenDisposition::ReadOnly);

        // The changes to capture are those written after the baseline recorded its own time.
        int64_t baselineTime = 0;
        std::optional<std::string> baselineTimeString = SQLite::MetadataTable::TryGetNamedValue<std::string>(baselineConnection, s_MetadataValueName_DeltaBaselineTime);
        if (baselineTimeString && !baselineTimeString->empty())
        {
            baselineTime = std::stoll(baselineTimeString.value());
        }

        auto changedPackages = V2_0::PackageUpdateTrackingTable::GetUpdatesSince(connection, baselineTime, m_trackingRemovalBehavior);
        auto removedPackages = V2_0::PackageUpdateTrackingTable::GetRemovalsSince(connection, baselineTime, m_trackingRemovalBehavior);

        Delta::Generate(
            connection,
            baselineConnection,
            deltaOutputPath,
            GetVersion(),
            changedPackages,
            removedPackages);
    }
}
