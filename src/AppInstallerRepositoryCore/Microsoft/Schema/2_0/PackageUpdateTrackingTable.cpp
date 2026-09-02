// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#include "pch.h"
#include "PackageUpdateTrackingTable.h"
#include <winget/PackageVersionDataManifest.h>
#include <winget/SQLiteStatementBuilder.h>

using namespace AppInstaller::SQLite;

namespace AppInstaller::Repository::Microsoft::Schema::V2_0
{
    using namespace std::string_view_literals;
    static constexpr std::string_view s_PUTT_Table_Name = "update_tracking"sv;
    static constexpr std::string_view s_PUTT_WriteTimeIndex_Name = "update_tracking_write_idx"sv;
    static constexpr std::string_view s_PUTT_Package = "package"sv;
    static constexpr std::string_view s_PUTT_WriteTime = "write_time"sv;
    static constexpr std::string_view s_PUTT_Manifest = "manifest"sv;
    static constexpr std::string_view s_PUTT_Hash = "hash"sv;
    static constexpr std::string_view s_PUTT_IsRemoved = "is_removed"sv;

    std::string_view PackageUpdateTrackingTable::TableName()
    {
        return s_PUTT_Table_Name;
    }

    void PackageUpdateTrackingTable::Create(SQLite::Connection& connection, RemovalBehavior removals)
    {
        using namespace Builder;

        StatementBuilder builder;
        builder.CreateTable(s_PUTT_Table_Name).BeginColumns();

        builder.Column(IntegerPrimaryKey());
        builder.Column(ColumnBuilder(s_PUTT_Package, Type::Text).NotNull());
        builder.Column(ColumnBuilder(s_PUTT_WriteTime, Type::Int64).NotNull());
        builder.Column(ColumnBuilder(s_PUTT_Manifest, Type::Blob));
        builder.Column(ColumnBuilder(s_PUTT_Hash, Type::Blob));

        if (removals == RemovalBehavior::Record)
        {
            builder.Column(ColumnBuilder(s_PUTT_IsRemoved, Type::Int64).NotNull().Default(0));
        }

        builder.EndColumns();

        builder.Execute(connection);

        StatementBuilder indexBuilder;
        indexBuilder.CreateIndex(s_PUTT_WriteTimeIndex_Name).On(s_PUTT_Table_Name).Columns(s_PUTT_WriteTime);
        indexBuilder.Execute(connection);
    }

    void PackageUpdateTrackingTable::EnsureExists(SQLite::Connection& connection, RemovalBehavior removals)
    {
        if (!Exists(connection))
        {
            Create(connection, removals);
        }
    }

    void PackageUpdateTrackingTable::Drop(SQLite::Connection& connection)
    {
        Builder::StatementBuilder dropTableBuilder;
        dropTableBuilder.DropTable(s_PUTT_Table_Name);

        dropTableBuilder.Execute(connection);
    }

    bool PackageUpdateTrackingTable::Exists(const SQLite::Connection& connection)
    {
        Builder::StatementBuilder builder;
        builder.Select(Builder::RowCount).From(Builder::Schema::MainTable).
            Where(Builder::Schema::TypeColumn).Equals(Builder::Schema::Type_Table).And(Builder::Schema::NameColumn).Equals(s_PUTT_Table_Name);

        Statement statement = builder.Prepare(connection);
        THROW_HR_IF(E_UNEXPECTED, !statement.Step());
        return statement.GetColumn<int64_t>(0) != 0;
    }

    void PackageUpdateTrackingTable::Update(SQLite::Connection& connection, const ISQLiteIndex* internalIndex, const std::string& packageIdentifier, RemovalBehavior removals, bool ensureTable)
    {
        if (ensureTable)
        {
            EnsureExists(connection, removals);
        }

        SearchRequest request;
        request.Inclusions.emplace_back(PackageMatchField::Id, MatchType::CaseInsensitive, packageIdentifier);
        auto result = internalIndex->Search(connection, request);

        if (result.Matches.empty())
        {
            if (removals == RemovalBehavior::Delete)
            {
                // Remove any existing package update row
                Builder::StatementBuilder deleteBuilder;
                deleteBuilder.DeleteFrom(s_PUTT_Table_Name).Where(s_PUTT_Package).LikeWithEscape(packageIdentifier);

                deleteBuilder.Execute(connection);
            }
            else
            {
                // Mark the package as removed rather than deleting the row, clearing the data columns.
                int64_t currentTime = Utility::GetCurrentUnixEpoch();

                Builder::StatementBuilder updateBuilder;
                updateBuilder.Update(s_PUTT_Table_Name).Set().
                    Column(s_PUTT_WriteTime).Equals(currentTime).
                    Column(s_PUTT_Manifest).AssignValue(nullptr).
                    Column(s_PUTT_Hash).AssignValue(nullptr).
                    Column(s_PUTT_IsRemoved).Equals(1).
                    Where(s_PUTT_Package).LikeWithEscape(packageIdentifier);
                updateBuilder.Execute(connection);

                if (connection.GetChanges() == 0)
                {
                    // The package was added and removed without an intervening tracking checkpoint,
                    // so there is no row to mark. Record the removal so that a delta built against an
                    // older baseline still learns that the package is gone.
                    Builder::StatementBuilder insertBuilder;
                    insertBuilder.InsertInto(s_PUTT_Table_Name).
                        Columns({ s_PUTT_Package, s_PUTT_WriteTime, s_PUTT_IsRemoved }).
                        Values(packageIdentifier, currentTime, 1);
                    insertBuilder.Execute(connection);
                }
            }
        }
        else
        {
            THROW_HR_IF(E_UNEXPECTED, result.Matches.size() != 1);

            // Insert or update the package row
            std::vector<ISQLiteIndex::VersionKey> versionKeys = internalIndex->GetVersionKeysById(connection, result.Matches[0].first);

            Manifest::PackageVersionDataManifest manifest;

            for (const auto& key : versionKeys)
            {
                Manifest::PackageVersionDataManifest::VersionData versionData{
                    key.VersionAndChannel,
                    internalIndex->GetPropertyByPrimaryId(connection, key.ManifestId, PackageVersionProperty::ArpMinVersion),
                    internalIndex->GetPropertyByPrimaryId(connection, key.ManifestId, PackageVersionProperty::ArpMaxVersion),
                    internalIndex->GetPropertyByPrimaryId(connection, key.ManifestId, PackageVersionProperty::RelativePath),
                    internalIndex->GetPropertyByPrimaryId(connection, key.ManifestId, PackageVersionProperty::ManifestSHA256Hash)
                };

                manifest.AddVersion(std::move(versionData));
            }

            std::string manifestString = manifest.Serialize();

            auto compressor = Manifest::PackageVersionDataManifest::CreateCompressor();
            std::vector<uint8_t> compressedManifest = compressor.Compress(manifestString);

            Utility::SHA256::HashBuffer manifestHash = Utility::SHA256::ComputeHash(compressedManifest);
            int64_t currentTime = Utility::GetCurrentUnixEpoch();

            // First attempt to update the row and then insert it if no modification occurred.
            Builder::StatementBuilder updateBuilder;
            updateBuilder.Update(s_PUTT_Table_Name).Set().
                Column(s_PUTT_WriteTime).Equals(currentTime).
                Column(s_PUTT_Manifest).Equals(compressedManifest).
                Column(s_PUTT_Hash).Equals(manifestHash);

            if (removals == RemovalBehavior::Record)
            {
                // Clear the flag in case this package was previously removed and is now being re-added.
                updateBuilder.Column(s_PUTT_IsRemoved).Equals(0);
            }

            updateBuilder.Where(s_PUTT_Package).LikeWithEscape(packageIdentifier);

            updateBuilder.Execute(connection);

            if (connection.GetChanges() == 0)
            {
                Builder::StatementBuilder insertBuilder;
                insertBuilder.InsertInto(s_PUTT_Table_Name).
                    Columns({ s_PUTT_Package, s_PUTT_WriteTime, s_PUTT_Manifest, s_PUTT_Hash }).
                    Values(packageIdentifier, currentTime, compressedManifest, manifestHash);

                insertBuilder.Execute(connection);
            }
        }
    }

    bool PackageUpdateTrackingTable::CheckConsistency(const SQLite::Connection& connection, ISQLiteIndex* internalIndex, RemovalBehavior removals, bool log)
    {
        bool result = true;

        // Ensure that all data in the update table matches the internal index
        for (const PackageData& packageData : GetUpdatesSince(connection, 0, removals))
        {
            auto manifestHash = Utility::SHA256::ComputeHash(packageData.Manifest);
            if (!Utility::SHA256::AreEqual(packageData.Hash, manifestHash))
            {
                if (!log)
                {
                    return false;
                }

                result = false;
                AICLI_LOG(Repo, Info, << "  [INVALID] value [" << s_PUTT_Hash << "] in table [" << s_PUTT_Table_Name <<
                    "] at row [" << packageData.RowID << "]; the hash of the manifest value does not match the hash in the row");
            }

            SearchRequest request;
            request.Inclusions.emplace_back(PackageMatchField::Id, MatchType::CaseInsensitive, packageData.PackageIdentifier);

            if (internalIndex->Search(connection, request).Matches.empty())
            {
                if (!log)
                {
                    return false;
                }

                result = false;
                AICLI_LOG(Repo, Info, << "  [INVALID] value [" << s_PUTT_Package << "] in table [" << s_PUTT_Table_Name <<
                    "] at row [" << packageData.RowID << "]; the package [" << packageData.PackageIdentifier << "] was not found in the internal index");
            }
        }

        // Any package recorded as removed must no longer be in the internal index
        for (const std::string& packageIdentifier : GetRemovalsSince(connection, 0, removals))
        {
            SearchRequest request;
            request.Inclusions.emplace_back(PackageMatchField::Id, MatchType::CaseInsensitive, packageIdentifier);

            if (!internalIndex->Search(connection, request).Matches.empty())
            {
                if (!log)
                {
                    return false;
                }

                result = false;
                AICLI_LOG(Repo, Info, << "  [INVALID] value [" << s_PUTT_Package << "] in table [" << s_PUTT_Table_Name <<
                    "]; the package [" << packageIdentifier << "] is marked as removed but is present in the internal index");
            }
        }

        // Ensure that all packages in the internal index are present in the update table
        Builder::StatementBuilder builder;
        builder.Select(Builder::RowCount).From(s_PUTT_Table_Name).Where(s_PUTT_Package).Like(Builder::Unbound).Escape(EscapeCharForLike);

        if (removals == RemovalBehavior::Record)
        {
            builder.And(s_PUTT_IsRemoved).Equals(0);
        }

        Statement select = builder.Prepare(connection);

        for (const auto& packageMatch : internalIndex->Search(connection, {}).Matches)
        {
            std::vector<ISQLiteIndex::VersionKey> versionKeys = internalIndex->GetVersionKeysById(connection, packageMatch.first);
            ISQLiteIndex::VersionKey& latestVersionKey = versionKeys[0];

            std::string packageIdentifier = internalIndex->GetPropertyByPrimaryId(connection, latestVersionKey.ManifestId, PackageVersionProperty::Id).value();

            select.Reset();
            select.Bind(1, packageIdentifier);
            select.Step();

            if (select.GetColumn<int64_t>(0) != 1)
            {
                if (!log)
                {
                    return false;
                }

                result = false;
                AICLI_LOG(Repo, Info, << "  [INVALID] value [" << packageIdentifier << "] in the internal index was not found as a non-removed entry in [" << s_PUTT_Table_Name << "]");
            }
        }

        return result;
    }

    std::vector<PackageUpdateTrackingTable::PackageData> PackageUpdateTrackingTable::GetUpdatesSince(const SQLite::Connection& connection, int64_t updateBaseTime, RemovalBehavior removals)
    {
        Builder::StatementBuilder builder;
        builder.Select({ RowIDName, s_PUTT_Package, s_PUTT_WriteTime, s_PUTT_Manifest, s_PUTT_Hash }).
            From(s_PUTT_Table_Name).Where(s_PUTT_WriteTime).IsGreaterThanOrEqualTo(updateBaseTime);

        if (removals == RemovalBehavior::Record)
        {
            // Removals are reported separately, so that this remains the set of packages that
            // have data to write out, exactly as it is when removals delete their row.
            builder.And(s_PUTT_IsRemoved).Equals(0);
        }

        Statement select = builder.Prepare(connection);

        std::vector<PackageData> result;

        while (select.Step())
        {
            PackageData item;
            item.RowID = select.GetColumn<rowid_t>(0);
            item.PackageIdentifier = select.GetColumn<std::string>(1);
            item.WriteTime = select.GetColumn<int64_t>(2);
            item.Manifest = select.GetColumn<blob_t>(3);
            item.Hash = select.GetColumn<blob_t>(4);

            result.emplace_back(std::move(item));
        }

        return result;
    }

    std::vector<std::string> PackageUpdateTrackingTable::GetRemovalsSince(const SQLite::Connection& connection, int64_t updateBaseTime, RemovalBehavior removals)
    {
        std::vector<std::string> result;

        if (removals == RemovalBehavior::Delete)
        {
            // Removals delete their row, so there is nothing to report.
            return result;
        }

        Builder::StatementBuilder builder;
        builder.Select(s_PUTT_Package).From(s_PUTT_Table_Name).
            Where(s_PUTT_WriteTime).IsGreaterThanOrEqualTo(updateBaseTime).
            And(s_PUTT_IsRemoved).Equals(1);

        Statement select = builder.Prepare(connection);

        while (select.Step())
        {
            result.emplace_back(select.GetColumn<std::string>(0));
        }

        return result;
    }

    SQLite::blob_t PackageUpdateTrackingTable::GetDataHash(const SQLite::Connection& connection, const std::string& packageIdentifier)
    {
        Builder::StatementBuilder builder;
        builder.Select(s_PUTT_Hash).From(s_PUTT_Table_Name).Where(s_PUTT_Package).LikeWithEscape(packageIdentifier);

        Statement select = builder.Prepare(connection);

        THROW_HR_IF(E_NOT_SET, !select.Step());

        return select.GetColumn<SQLite::blob_t>(0);
    }

    void PackageUpdateTrackingTable::AddIsRemovedColumn(SQLite::Connection& connection)
    {
        // The table is created on demand, so an index that has never had a manifest written
        // to it will not have one yet. It will be created with the column when it is needed.
        if (!Exists(connection))
        {
            return;
        }

        Builder::StatementBuilder builder;
        builder.AlterTable(s_PUTT_Table_Name).Add(Builder::ColumnBuilder(s_PUTT_IsRemoved, Builder::Type::Int64).NotNull().Default(0));
        builder.Execute(connection);
    }
}
