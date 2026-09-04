// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#include "pch.h"
#include "PackageUpdateTrackingTable.h"
#include "Microsoft/Schema/1_0/IdTable.h"
#include <winget/PackageVersionDataManifest.h>
#include <winget/SQLiteStatementBuilder.h>
#include <AppInstallerStrings.h>

#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace AppInstaller::SQLite;

namespace AppInstaller::Repository::Microsoft::Schema::V2_0
{
    using namespace std::string_view_literals;
    static constexpr std::string_view s_PUTT_Table_Name = "update_tracking"sv;
    static constexpr std::string_view s_PUTT_WriteTimeIndex_Name = "update_tracking_write_idx"sv;
    static constexpr std::string_view s_PUTT_LiveIndex_Name = "update_tracking_live_idx"sv;
    static constexpr std::string_view s_PUTT_Package = "package"sv;
    static constexpr std::string_view s_PUTT_WriteTime = "write_time"sv;
    static constexpr std::string_view s_PUTT_Manifest = "manifest"sv;
    static constexpr std::string_view s_PUTT_Hash = "hash"sv;
    static constexpr std::string_view s_PUTT_IsRemoved = "is_removed"sv;
    static constexpr std::string_view s_PUTT_PackageRowId = "package_rowid"sv;

    namespace
    {
        // Finds the rowid that the package occupies in the index.
        //
        // The identity of a package is the rowid of its `ids` row, not the identifier string.
        // AddManifest calls IdTable::EnsureExists with overwriteLikeMatch, so identifiers that
        // match under LIKE collapse onto a single rowid and the stored string is replaced by the
        // most recent casing. Matching by rowid therefore inherits the index's own notion of
        // identity, and is stable against that string changing underneath us.
        std::optional<SQLite::rowid_t> GetPackageRowIdInIndex(const SQLite::Connection& connection, const std::string& packageIdentifier)
        {
            return V1_0::IdTable::SelectIdByValue(connection, packageIdentifier, true);
        }

        // Determines whether the package currently has a row that is not marked as removed.
        bool HasLiveRow(const SQLite::Connection& connection, const std::string& packageIdentifier)
        {
            Builder::StatementBuilder builder;
            builder.Select(Builder::RowCount).From(s_PUTT_Table_Name).
                Where(s_PUTT_Package).LikeWithEscape(packageIdentifier).
                And(s_PUTT_IsRemoved).Equals(0);

            Statement statement = builder.Prepare(connection);
            THROW_HR_IF(E_UNEXPECTED, !statement.Step());
            return statement.GetColumn<int64_t>(0) != 0;
        }
    }

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

            // The rowid the package occupies in the index, which is what a delta is keyed on.
            // 0 means unknown, which only arises for a removal of a package that was never
            // tracked as present; generation resolves removals against the baseline anyway.
            builder.Column(ColumnBuilder(s_PUTT_PackageRowId, Type::Int64).NotNull().Default(0));
        }

        builder.EndColumns();

        builder.Execute(connection);

        StatementBuilder indexBuilder;
        indexBuilder.CreateIndex(s_PUTT_WriteTimeIndex_Name).On(s_PUTT_Table_Name).Columns(s_PUTT_WriteTime);
        indexBuilder.Execute(connection);

        if (removals == RemovalBehavior::Record)
        {
            CreateLiveRowIndex(connection);
        }
    }

    void PackageUpdateTrackingTable::CreateLiveRowIndex(SQLite::Connection& connection)
    {
        // A package occupies exactly one rowid at a time, so at most one row per rowid can be
        // live. Tombstones are excluded because a rowid vacated by one package can be taken by
        // another, leaving the old package's tombstone and the new package's live row sharing it.
        //
        // The constraint is deliberately on the rowid rather than the identifier: a unique index
        // cannot use LIKE, and no available collation matches it. NOCASE is ASCII-only, while
        // LIKE here is the ICU implementation registered by sqlite3IcuInit, so NOCASE would
        // disagree with every other accessor on non-ASCII identifiers. An ICU collation cannot be
        // used either, since it would bake the ICU version into a published index file and make
        // the table unreadable to any connection that had not registered it.
        Builder::StatementBuilder builder;
        builder.CreateUniqueIndex(s_PUTT_LiveIndex_Name).On(s_PUTT_Table_Name).Columns(s_PUTT_PackageRowId).
            Where(s_PUTT_IsRemoved).EqualsLiteral(0);
        builder.Execute(connection);
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
                // Only the live row is marked; earlier tombstones refer to rowids the package has
                // already vacated and must be preserved so that a delta learns about each of them.
                // The package rowid is carried forward untouched, since the package is already gone
                // from the index and can no longer be looked up there.
                int64_t currentTime = Utility::GetCurrentUnixEpoch();

                Builder::StatementBuilder updateBuilder;
                updateBuilder.Update(s_PUTT_Table_Name).Set().
                    Column(s_PUTT_WriteTime).Equals(currentTime).
                    Column(s_PUTT_Manifest).AssignValue(nullptr).
                    Column(s_PUTT_Hash).AssignValue(nullptr).
                    Column(s_PUTT_IsRemoved).Equals(1).
                    Where(s_PUTT_Package).LikeWithEscape(packageIdentifier).
                    And(s_PUTT_IsRemoved).Equals(0);
                updateBuilder.Execute(connection);

                if (connection.GetChanges() == 0)
                {
                    // The package was added and removed without an intervening tracking checkpoint,
                    // so there is no row to mark. Record the removal so that a delta built against an
                    // older baseline still learns that the package is gone. The rowid is unknown
                    // because the package is no longer in the index; generation does not need it,
                    // as it resolves a removal against the baseline by identifier.
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

            // The rowid the package occupies is the identity that a delta is keyed on, so it is
            // recorded alongside the data. It is resolved here rather than at removal time because
            // the package is still in the index at this point.
            SQLite::rowid_t packageRowId = 0;

            if (removals == RemovalBehavior::Record)
            {
                std::optional<SQLite::rowid_t> indexRowId = GetPackageRowIdInIndex(connection, packageIdentifier);
                THROW_HR_IF(E_NOT_VALID_STATE, !indexRowId);
                packageRowId = indexRowId.value();
            }

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

            if (removals == RemovalBehavior::Record)
            {
                // Match on the rowid as well, so that a re-add only revives the tombstone that
                // refers to the rowid the package now occupies. A package removed and re-added
                // elsewhere leaves its old tombstone intact, which is what tells a delta to
                // suppress the baseline row it still holds. Restricting by rowid also prevents
                // this update from overwriting a different package's tombstone that happens to
                // name the same rowid.
                updateBuilder.And(s_PUTT_PackageRowId).Equals(packageRowId);
            }

            updateBuilder.Execute(connection);

            if (connection.GetChanges() == 0)
            {
                Builder::StatementBuilder insertBuilder;
                insertBuilder.InsertInto(s_PUTT_Table_Name).BeginColumns();

                insertBuilder.Column(s_PUTT_Package);
                insertBuilder.Column(s_PUTT_WriteTime);
                insertBuilder.Column(s_PUTT_Manifest);
                insertBuilder.Column(s_PUTT_Hash);

                if (removals == RemovalBehavior::Record)
                {
                    insertBuilder.Column(s_PUTT_PackageRowId);
                }

                insertBuilder.EndColumns().BeginValues();

                insertBuilder.Value(packageIdentifier);
                insertBuilder.Value(currentTime);
                insertBuilder.Value(compressedManifest);
                insertBuilder.Value(manifestHash);

                if (removals == RemovalBehavior::Record)
                {
                    insertBuilder.Value(packageRowId);
                }

                insertBuilder.EndValues();

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
            // A package that was removed and later re-added keeps the tombstone for the rowid it
            // vacated alongside a live row for the rowid it now occupies. Its presence in the
            // index is therefore expected, and only the live row describes it.
            if (HasLiveRow(connection, packageIdentifier))
            {
                continue;
            }

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

        // Every live row must name the rowid that the package actually occupies, since that is the
        // identity a delta is keyed on. A disagreement means either this table or the rowid
        // pinning performed during packaging has drifted.
        if (removals == RemovalBehavior::Record)
        {
            for (const PackageData& packageData : GetUpdatesSince(connection, 0, removals))
            {
                std::optional<SQLite::rowid_t> indexRowId = GetPackageRowIdInIndex(connection, packageData.PackageIdentifier);

                if (!indexRowId || indexRowId.value() != packageData.PackageRowId)
                {
                    if (!log)
                    {
                        return false;
                    }

                    result = false;
                    AICLI_LOG(Repo, Info, << "  [INVALID] value [" << s_PUTT_PackageRowId << "] in table [" << s_PUTT_Table_Name <<
                        "] at row [" << packageData.RowID << "]; the package [" << packageData.PackageIdentifier << "] records rowid [" <<
                        packageData.PackageRowId << "] but occupies [" << (indexRowId ? std::to_string(indexRowId.value()) : "none") << "]");
                }
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
        bool recordingRemovals = (removals == RemovalBehavior::Record);

        Builder::StatementBuilder builder;

        if (recordingRemovals)
        {
            builder.Select({ RowIDName, s_PUTT_Package, s_PUTT_WriteTime, s_PUTT_Manifest, s_PUTT_Hash, s_PUTT_PackageRowId });
        }
        else
        {
            builder.Select({ RowIDName, s_PUTT_Package, s_PUTT_WriteTime, s_PUTT_Manifest, s_PUTT_Hash });
        }

        builder.From(s_PUTT_Table_Name).Where(s_PUTT_WriteTime).IsGreaterThanOrEqualTo(updateBaseTime);

        if (recordingRemovals)
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

            if (recordingRemovals)
            {
                item.PackageRowId = select.GetColumn<rowid_t>(5);
            }

            result.emplace_back(std::move(item));
        }

        return result;
    }

    std::set<std::string> PackageUpdateTrackingTable::GetRemovalsSince(const SQLite::Connection& connection, int64_t updateBaseTime, RemovalBehavior removals)
    {
        std::set<std::string> result;

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
            result.emplace(Utility::FoldCase(static_cast<std::string_view>(select.GetColumn<std::string>(0))));
        }

        return result;
    }

    SQLite::blob_t PackageUpdateTrackingTable::GetDataHash(const SQLite::Connection& connection, const std::string& packageIdentifier, RemovalBehavior removals)
    {
        Builder::StatementBuilder builder;
        builder.Select(s_PUTT_Hash).From(s_PUTT_Table_Name).Where(s_PUTT_Package).LikeWithEscape(packageIdentifier);

        if (removals == RemovalBehavior::Record)
        {
            // A tombstone has its hash cleared, so restrict to the live row or this would be
            // liable to return a null hash for a package that was removed and re-added.
            builder.And(s_PUTT_IsRemoved).Equals(0);
        }

        Statement select = builder.Prepare(connection);

        THROW_HR_IF(E_NOT_SET, !select.Step());

        return select.GetColumn<SQLite::blob_t>(0);
    }

    void PackageUpdateTrackingTable::AddRemovalTrackingColumns(SQLite::Connection& connection)
    {
        // The table is created on demand, so an index that has never had a manifest written
        // to it will not have one yet. It will be created with the columns when it is needed.
        if (!Exists(connection))
        {
            return;
        }

        Builder::StatementBuilder isRemovedBuilder;
        isRemovedBuilder.AlterTable(s_PUTT_Table_Name).Add(Builder::ColumnBuilder(s_PUTT_IsRemoved, Builder::Type::Int64).NotNull().Default(0));
        isRemovedBuilder.Execute(connection);

        Builder::StatementBuilder packageRowIdBuilder;
        packageRowIdBuilder.AlterTable(s_PUTT_Table_Name).Add(Builder::ColumnBuilder(s_PUTT_PackageRowId, Builder::Type::Int64).NotNull().Default(0));
        packageRowIdBuilder.Execute(connection);

        // Backfill the rowid for the rows already present. Every one of them is live: 2.0 deletes
        // the row when a package is removed, so a table being migrated has no tombstones and every
        // package it names is still in the index.
        Builder::StatementBuilder selectBuilder;
        selectBuilder.Select({ RowIDName, s_PUTT_Package }).From(s_PUTT_Table_Name);

        std::vector<std::pair<rowid_t, std::string>> rows;

        {
            Statement select = selectBuilder.Prepare(connection);

            while (select.Step())
            {
                rows.emplace_back(select.GetColumn<rowid_t>(0), select.GetColumn<std::string>(1));
            }
        }

        Builder::StatementBuilder updateBuilder;
        updateBuilder.Update(s_PUTT_Table_Name).Set().
            Column(s_PUTT_PackageRowId).Equals(Builder::Unbound).
            Where(RowIDName).Equals(Builder::Unbound);

        Statement update = updateBuilder.Prepare(connection);

        for (const auto& row : rows)
        {
            std::optional<SQLite::rowid_t> packageRowId = GetPackageRowIdInIndex(connection, row.second);
            THROW_HR_IF(E_NOT_VALID_STATE, !packageRowId);

            update.Reset();
            update.Bind(1, packageRowId.value());
            update.Bind(2, row.first);
            update.Execute();
        }

        CreateLiveRowIndex(connection);
    }
}
