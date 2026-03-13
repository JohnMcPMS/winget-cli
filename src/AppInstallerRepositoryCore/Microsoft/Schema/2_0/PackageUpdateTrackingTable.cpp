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

    void PackageUpdateTrackingTable::Create(SQLite::Connection& connection)
    {
        using namespace Builder;

        StatementBuilder builder;
        builder.CreateTable(s_PUTT_Table_Name).BeginColumns();

        builder.Column(IntegerPrimaryKey());
        builder.Column(ColumnBuilder(s_PUTT_Package, Type::Text).NotNull());
        builder.Column(ColumnBuilder(s_PUTT_WriteTime, Type::Int64).NotNull());
        builder.Column(ColumnBuilder(s_PUTT_Manifest, Type::Blob));
        builder.Column(ColumnBuilder(s_PUTT_Hash, Type::Blob));
        builder.Column(ColumnBuilder(s_PUTT_IsRemoved, Type::Int64).NotNull().WithDefaultValue(0));

        builder.EndColumns();

        builder.Execute(connection);

        StatementBuilder indexBuilder;
        indexBuilder.CreateIndex(s_PUTT_WriteTimeIndex_Name).On(s_PUTT_Table_Name).Columns(s_PUTT_WriteTime);
        indexBuilder.Execute(connection);
    }

    void PackageUpdateTrackingTable::EnsureExists(SQLite::Connection& connection)
    {
        if (!Exists(connection))
        {
            Create(connection);
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

    void PackageUpdateTrackingTable::Update(SQLite::Connection& connection, const ISQLiteIndex* internalIndex, const std::string& packageIdentifier, bool ensureTable)
    {
        if (ensureTable)
        {
            EnsureExists(connection);
        }

        SearchRequest request;
        request.Inclusions.emplace_back(PackageMatchField::Id, MatchType::CaseInsensitive, packageIdentifier);
        auto result = internalIndex->Search(connection, request);

        if (result.Matches.empty())
        {
            // Mark the package as removed rather than deleting the row; clear the data columns.
            int64_t currentTime = Utility::GetCurrentUnixEpoch();

            Builder::StatementBuilder updateBuilder;
            updateBuilder.Update(s_PUTT_Table_Name).Set().
                Column(s_PUTT_WriteTime).Equals(currentTime).
                Column(s_PUTT_Manifest).Equals(nullptr).
                Column(s_PUTT_Hash).Equals(nullptr).
                Column(s_PUTT_IsRemoved).Equals(1).
                Where(s_PUTT_Package).LikeWithEscape(packageIdentifier);
            updateBuilder.Execute(connection);

            if (connection.GetChanges() == 0)
            {
                // Package was never tracked (added and removed before any tracking checkpoint); record its removal.
                Builder::StatementBuilder insertBuilder;
                insertBuilder.InsertInto(s_PUTT_Table_Name).
                    Columns({ s_PUTT_Package, s_PUTT_WriteTime, s_PUTT_IsRemoved }).
                    Values(packageIdentifier, currentTime, 1);
                insertBuilder.Execute(connection);
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
            // Also clears is_removed in case this package was previously removed and is being re-added.
            Builder::StatementBuilder updateBuilder;
            updateBuilder.Update(s_PUTT_Table_Name).Set().
                Column(s_PUTT_WriteTime).Equals(currentTime).
                Column(s_PUTT_Manifest).Equals(compressedManifest).
                Column(s_PUTT_Hash).Equals(manifestHash).
                Column(s_PUTT_IsRemoved).Equals(0).
                Where(s_PUTT_Package).LikeWithEscape(packageIdentifier);

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

    bool PackageUpdateTrackingTable::CheckConsistency(const SQLite::Connection& connection, ISQLiteIndex* internalIndex, bool log)
    {
        bool result = true;

        // Ensure that all non-removed data in the update table matches the internal index
        for (const PackageData& packageData : GetUpdatesSince(connection, 0))
        {
            if (packageData.IsRemoved)
            {
                // Removed packages should not be in the internal index
                SearchRequest request;
                request.Inclusions.emplace_back(PackageMatchField::Id, MatchType::CaseInsensitive, packageData.PackageIdentifier);
                if (!internalIndex->Search(connection, request).Matches.empty())
                {
                    if (!log)
                    {
                        return false;
                    }
                    result = false;
                    AICLI_LOG(Repo, Info, << "  [INVALID] value [" << s_PUTT_Package << "] in table [" << s_PUTT_Table_Name <<
                        "] at row [" << packageData.RowID << "]; package [" << packageData.PackageIdentifier << "] is marked removed but still exists in the internal index");
                }
                continue;
            }

            if (packageData.Manifest.empty())
            {
                if (!log)
                {
                    return false;
                }
                result = false;
                AICLI_LOG(Repo, Info, << "  [INVALID] value [" << s_PUTT_Manifest << "] in table [" << s_PUTT_Table_Name <<
                    "] at row [" << packageData.RowID << "]; manifest blob is empty for non-removed package");
                continue;
            }

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

        // Ensure that all packages in the internal index are present in the update table (as non-removed)
        Builder::StatementBuilder builder;
        builder.Select(Builder::RowCount).From(s_PUTT_Table_Name).
            Where(s_PUTT_Package).Like(Builder::Unbound).Escape(EscapeCharForLike).
            And(s_PUTT_IsRemoved).Equals(0);

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

    std::vector<PackageUpdateTrackingTable::PackageData> PackageUpdateTrackingTable::GetUpdatesSince(const SQLite::Connection& connection, int64_t updateBaseTime)
    {
        Builder::StatementBuilder builder;
        builder.Select({ RowIDName, s_PUTT_Package, s_PUTT_WriteTime, s_PUTT_Manifest, s_PUTT_Hash, s_PUTT_IsRemoved }).
            From(s_PUTT_Table_Name).Where(s_PUTT_WriteTime).IsGreaterThanOrEqualTo(updateBaseTime);

        Statement select = builder.Prepare(connection);

        std::vector<PackageData> result;

        while (select.Step())
        {
            PackageData item;
            item.RowID = select.GetColumn<rowid_t>(0);
            item.PackageIdentifier = select.GetColumn<std::string>(1);
            item.WriteTime = select.GetColumn<int64_t>(2);
            item.IsRemoved = (select.GetColumn<int64_t>(5) != 0);

            if (!item.IsRemoved)
            {
                item.Manifest = select.GetColumn<blob_t>(3);
                item.Hash = select.GetColumn<blob_t>(4);
            }

            result.emplace_back(std::move(item));
        }

        return result;
    }

    SQLite::blob_t PackageUpdateTrackingTable::GetDataHash(const SQLite::Connection& connection, const std::string& packageIdentifier)
    {
        Builder::StatementBuilder builder;
        builder.Select(s_PUTT_Hash).From(s_PUTT_Table_Name).
            Where(s_PUTT_Package).LikeWithEscape(packageIdentifier).
            And(s_PUTT_IsRemoved).Equals(0);

        Statement select = builder.Prepare(connection);

        THROW_HR_IF(E_NOT_SET, !select.Step());

        return select.GetColumn<SQLite::blob_t>(0);
    }

    void PackageUpdateTrackingTable::EnsureIsRemovedColumn(SQLite::Connection& connection)
    {
        // Use PRAGMA table_info to check whether is_removed already exists.
        SQLite::Statement info = SQLite::Statement::Create(connection, "PRAGMA table_info(update_tracking)");
        bool hasColumn = false;
        while (info.Step())
        {
            if (info.GetColumn<std::string>(1) == std::string{ s_PUTT_IsRemoved })
            {
                hasColumn = true;
                break;
            }
        }

        if (!hasColumn)
        {
            SQLite::Statement alter = SQLite::Statement::Create(connection,
                "ALTER TABLE update_tracking ADD COLUMN is_removed INTEGER NOT NULL DEFAULT 0");
            alter.Execute();
        }
    }
}
