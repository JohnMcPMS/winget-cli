// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#include "pch.h"
#include <winget/SQLiteMetadataTable.h>
#include <winget/SQLiteWrapper.h>
#include "Microsoft/Schema/2_0/Interface.h"

#include "Microsoft/Schema/2_0/PackagesTable.h"

#include "Microsoft/Schema/2_0/TagsTable.h"
#include "Microsoft/Schema/2_0/CommandsTable.h"
#include "Microsoft/Schema/2_0/PackageFamilyNameTable.h"
#include "Microsoft/Schema/2_0/ProductCodeTable.h"
#include "Microsoft/Schema/2_0/NormalizedPackageNameTable.h"
#include "Microsoft/Schema/2_0/NormalizedPackagePublisherTable.h"
#include "Microsoft/Schema/2_0/UpgradeCodeTable.h"

#include "Microsoft/Schema/2_0/SearchResultsTable.h"
#include "Microsoft/Schema/2_0/PackageUpdateTrackingTable.h"
#include "Microsoft/Schema/1_0/IdTable.h"

#include <winget/PackageVersionDataManifest.h>


namespace AppInstaller::Repository::Microsoft::Schema::V2_0
{
    namespace anon
    {
        // Folds the values of the fields that are stored folded.
        void FoldPackageMatchFilters(std::vector<PackageMatchFilter>& filters)
        {
            for (auto& filter : filters)
            {
                if ((filter.Field == PackageMatchField::PackageFamilyName || filter.Field == PackageMatchField::ProductCode || filter.Field == PackageMatchField::UpgradeCode) &&
                    filter.Type == MatchType::Exact)
                {
                    filter.Value = Utility::FoldCase(filter.Value);
                }
            }
        }

        // Update NormalizedNameAndPublisher with normalization and folding
        // Returns true if the normalized name contains normalization field of fieldsToInclude
        bool UpdateNormalizedNameAndPublisher(
            PackageMatchFilter& filter,
            const Utility::NameNormalizer& normalizer,
            Utility::NormalizationField fieldsToInclude)
        {
            Utility::NormalizedName normalized = normalizer.Normalize(Utility::FoldCase(filter.Value), Utility::FoldCase(filter.Additional.value()));
            filter.Value = normalized.GetNormalizedName(fieldsToInclude);
            filter.Additional = normalized.Publisher();
            return WI_AreAllFlagsSet(normalized.GetNormalizedFields(), fieldsToInclude);
        }

        bool UpdatePackageMatchFilters(
            std::vector<PackageMatchFilter>& filters,
            const Utility::NameNormalizer& normalizer,
            Utility::NormalizationField normalizedNameFieldsToFilter = Utility::NormalizationField::None)
        {
            bool normalizedNameFieldsFound = false;
            for (auto itr = filters.begin(); itr != filters.end();)
            {
                if (itr->Field == PackageMatchField::NormalizedNameAndPublisher && itr->Type == MatchType::Exact)
                {
                    if (!UpdateNormalizedNameAndPublisher(*itr, normalizer, normalizedNameFieldsToFilter))
                    {
                        // If not matched, this package match filter will be removed.
                        // For example, if caller is trying to search with arch info only, values without arch will be removed from search.
                        itr = filters.erase(itr);
                        continue;
                    }

                    normalizedNameFieldsFound = true;
                }

                ++itr;
            }

            return normalizedNameFieldsFound;
        }
    }

    namespace anon
    {
        // Executes a raw SQL statement on a connection using the statement builder mechanism.
        void ExecuteSQL(SQLite::Connection& connection, std::string_view sql)
        {
            SQLite::Statement stmt = SQLite::Statement::Create(connection, sql);
            stmt.Execute();
        }

        // Creates all delta tables in the delta connection.
        void CreateDeltaSchema(SQLite::Connection& deltaConn)
        {
            ExecuteSQL(deltaConn, R"(
                CREATE TABLE IF NOT EXISTS delta_packages (
                    rowid INTEGER PRIMARY KEY,
                    id TEXT NOT NULL,
                    name TEXT NOT NULL,
                    moniker TEXT,
                    latest_version TEXT NOT NULL,
                    arp_min_version TEXT,
                    arp_max_version TEXT,
                    hash BLOB,
                    is_removed INTEGER NOT NULL DEFAULT 0
                )
            )");

            // SystemReference string tables (value + package_id, no separate id)
            static constexpr std::pair<std::string_view, std::string_view> s_SysRefTables[] = {
                { "pfns2",            "pfn" },
                { "productcodes2",    "productcode" },
                { "norm_names2",      "norm_name" },
                { "norm_publishers2", "norm_publisher" },
                { "upgradecodes2",    "upgradecode" },
            };
            for (const auto& [table, value] : s_SysRefTables)
            {
                std::string sql = "CREATE TABLE IF NOT EXISTS delta_" + std::string(table) +
                    " (" + std::string(value) + " TEXT NOT NULL, package INTEGER NOT NULL, " +
                    "is_removed INTEGER NOT NULL DEFAULT 0, " +
                    "PRIMARY KEY (" + std::string(value) + ", package)) WITHOUT ROWID";
                ExecuteSQL(deltaConn, sql);
            }

            // OneToMany data tables (rowid + value)
            static constexpr std::pair<std::string_view, std::string_view> s_OneToManyTables[] = {
                { "tags2",     "tag" },
                { "commands2", "command" },
            };
            for (const auto& [table, value] : s_OneToManyTables)
            {
                std::string sql = "CREATE TABLE IF NOT EXISTS delta_" + std::string(table) +
                    " (rowid INTEGER PRIMARY KEY, " + std::string(value) + " TEXT NOT NULL)";
                ExecuteSQL(deltaConn, sql);
            }

            // OneToMany map tables (value_rowid + package_rowid)
            for (const auto& [table, value] : s_OneToManyTables)
            {
                std::string sql = "CREATE TABLE IF NOT EXISTS delta_" + std::string(table) + "_map" +
                    " (" + std::string(value) + " INTEGER NOT NULL, package INTEGER NOT NULL, " +
                    "is_removed INTEGER NOT NULL DEFAULT 0, " +
                    "PRIMARY KEY (" + std::string(value) + ", package)) WITHOUT ROWID";
                ExecuteSQL(deltaConn, sql);
            }
        }

        // Returns the rowid of a package in the baseline, or 0 if not found.
        SQLite::rowid_t GetBaselinePackageRowid(SQLite::Connection& baselineConn, const std::string& packageId)
        {
            SQLite::Builder::StatementBuilder builder;
            builder.Select(SQLite::RowIDName).From("packages").Where("id").Equals(packageId);
            SQLite::Statement stmt = builder.Prepare(baselineConn);
            if (stmt.Step())
            {
                return stmt.GetColumn<SQLite::rowid_t>(0);
            }
            return 0;
        }

        // Returns the max rowid in the packages table, or 0 if empty.
        SQLite::rowid_t GetMaxPackageRowid(SQLite::Connection& baselineConn)
        {
            SQLite::Statement stmt = SQLite::Statement::Create(baselineConn, "SELECT MAX(rowid) FROM packages");
            if (stmt.Step())
            {
                // MAX(rowid) returns NULL if table is empty
                if (!stmt.GetColumnIsNull(0))
                {
                    return stmt.GetColumn<SQLite::rowid_t>(0);
                }
            }
            return 0;
        }

        // Returns the max rowid in a data table (tags2 or commands2), or 0 if empty.
        SQLite::rowid_t GetMaxDataTableRowid(SQLite::Connection& baselineConn, std::string_view tableName)
        {
            std::string sql = "SELECT MAX(rowid) FROM " + std::string(tableName);
            SQLite::Statement stmt = SQLite::Statement::Create(baselineConn, sql);
            if (stmt.Step() && !stmt.GetColumnIsNull(0))
            {
                return stmt.GetColumn<SQLite::rowid_t>(0);
            }
            return 0;
        }

        // Returns the rowid in baseline data table for the given value string, or 0 if not present.
        SQLite::rowid_t GetBaselineDataTableRowid(SQLite::Connection& baselineConn, std::string_view tableName, std::string_view valueName, const std::string& value)
        {
            std::string sql = "SELECT rowid FROM " + std::string(tableName) + " WHERE " + std::string(valueName) + " = ?";
            SQLite::Statement stmt = SQLite::Statement::Create(baselineConn, sql);
            stmt.Bind(1, value);
            if (stmt.Step())
            {
                return stmt.GetColumn<SQLite::rowid_t>(0);
            }
            return 0;
        }

        // Inserts or finds a value in delta data table; returns the rowid (possibly from baseline).
        // baselineMaxRowid: the starting offset for new delta rowids.
        SQLite::rowid_t EnsureDeltaDataTableValue(
            SQLite::Connection& deltaConn,
            SQLite::Connection& baselineConn,
            std::string_view deltaTableName,
            std::string_view valueName,
            const std::string& value,
            SQLite::rowid_t& nextNewRowid)
        {
            // Check if the value is already in the baseline
            SQLite::rowid_t baselineRowid = GetBaselineDataTableRowid(baselineConn, std::string(deltaTableName).substr(6), valueName, value);
            if (baselineRowid != 0)
            {
                return baselineRowid;
            }

            // Check if already in the delta table
            std::string selectSql = "SELECT rowid FROM " + std::string(deltaTableName) + " WHERE " + std::string(valueName) + " = ?";
            SQLite::Statement selectStmt = SQLite::Statement::Create(deltaConn, selectSql);
            selectStmt.Bind(1, value);
            if (selectStmt.Step())
            {
                return selectStmt.GetColumn<SQLite::rowid_t>(0);
            }

            // Insert as a new entry
            SQLite::rowid_t newRowid = ++nextNewRowid;
            std::string insertSql = "INSERT INTO " + std::string(deltaTableName) + " (rowid, " + std::string(valueName) + ") VALUES (?, ?)";
            SQLite::Statement insertStmt = SQLite::Statement::Create(deltaConn, insertSql);
            insertStmt.Bind(1, newRowid);
            insertStmt.Bind(2, value);
            insertStmt.Execute();
            return newRowid;
        }

        // Processes a SystemReference table for a changed package.
        // Compares current values vs baseline values and records adds/removes.
        void ProcessDeltaSysRefTable(
            SQLite::Connection& deltaConn,
            SQLite::Connection& sourceConn,
            SQLite::Connection& baselineConn,
            std::string_view tableName,
            std::string_view valueName,
            SQLite::rowid_t packageRowid,
            const std::string& packageId)
        {
            UNREFERENCED_PARAMETER(packageId);
            std::string deltaTable = "delta_" + std::string(tableName);
            std::string primaryCol = "package";

            // Get current values from the new V2 index
            std::vector<std::string> currentValues;
            {
                std::string sql = "SELECT " + std::string(valueName) + " FROM " + std::string(tableName) + " WHERE " + primaryCol + " = ?";
                SQLite::Statement stmt = SQLite::Statement::Create(sourceConn, sql);
                stmt.Bind(1, packageRowid);
                while (stmt.Step())
                {
                    currentValues.push_back(stmt.GetColumn<std::string>(0));
                }
            }

            // Get baseline values
            std::vector<std::string> baselineValues;
            {
                std::string sql = "SELECT " + std::string(valueName) + " FROM " + std::string(tableName) + " WHERE " + primaryCol + " = ?";
                SQLite::Statement stmt = SQLite::Statement::Create(baselineConn, sql);
                stmt.Bind(1, packageRowid);
                while (stmt.Step())
                {
                    baselineValues.push_back(stmt.GetColumn<std::string>(0));
                }
            }

            // Find added values (in current but not baseline)
            for (const auto& val : currentValues)
            {
                if (std::find(baselineValues.begin(), baselineValues.end(), val) == baselineValues.end())
                {
                    std::string sql = "INSERT OR IGNORE INTO " + deltaTable +
                        " (" + std::string(valueName) + ", " + primaryCol + ", is_removed) VALUES (?, ?, 0)";
                    SQLite::Statement stmt = SQLite::Statement::Create(deltaConn, sql);
                    stmt.Bind(1, val);
                    stmt.Bind(2, packageRowid);
                    stmt.Execute();
                }
            }

            // Find removed values (in baseline but not current)
            for (const auto& val : baselineValues)
            {
                if (std::find(currentValues.begin(), currentValues.end(), val) == currentValues.end())
                {
                    std::string sql = "INSERT OR IGNORE INTO " + deltaTable +
                        " (" + std::string(valueName) + ", " + primaryCol + ", is_removed) VALUES (?, ?, 1)";
                    SQLite::Statement stmt = SQLite::Statement::Create(deltaConn, sql);
                    stmt.Bind(1, val);
                    stmt.Bind(2, packageRowid);
                    stmt.Execute();
                }
            }
        }

        // Processes a OneToMany table for a changed package.
        void ProcessDeltaOneToManyTable(
            SQLite::Connection& deltaConn,
            SQLite::Connection& sourceConn,
            SQLite::Connection& baselineConn,
            std::string_view tableName,
            std::string_view valueName,
            SQLite::rowid_t packageRowid,
            SQLite::rowid_t& nextNewDataRowid)
        {
            std::string deltaDataTable = "delta_" + std::string(tableName);
            std::string deltaMapTable = "delta_" + std::string(tableName) + "_map";
            std::string mapTable = std::string(tableName) + "_map";

            // Get current values via join (tags2_map JOIN tags2)
            std::vector<std::string> currentValues;
            {
                std::string sql = "SELECT t." + std::string(valueName) +
                    " FROM " + mapTable + " m JOIN " + std::string(tableName) + " t ON m." + std::string(valueName) + " = t.rowid" +
                    " WHERE m.package = ?";
                SQLite::Statement stmt = SQLite::Statement::Create(sourceConn, sql);
                stmt.Bind(1, packageRowid);
                while (stmt.Step())
                {
                    currentValues.push_back(stmt.GetColumn<std::string>(0));
                }
            }

            // Get baseline values via join
            std::vector<std::string> baselineValues;
            {
                std::string sql = "SELECT t." + std::string(valueName) +
                    " FROM " + mapTable + " m JOIN " + std::string(tableName) + " t ON m." + std::string(valueName) + " = t.rowid" +
                    " WHERE m.package = ?";
                SQLite::Statement stmt = SQLite::Statement::Create(baselineConn, sql);
                stmt.Bind(1, packageRowid);
                while (stmt.Step())
                {
                    baselineValues.push_back(stmt.GetColumn<std::string>(0));
                }
            }

            // Record added mappings (current but not baseline)
            for (const auto& val : currentValues)
            {
                if (std::find(baselineValues.begin(), baselineValues.end(), val) == baselineValues.end())
                {
                    SQLite::rowid_t dataRowid = EnsureDeltaDataTableValue(
                        deltaConn, baselineConn, deltaDataTable, valueName, val, nextNewDataRowid);

                    std::string sql = "INSERT OR IGNORE INTO " + deltaMapTable +
                        " (" + std::string(valueName) + ", package, is_removed) VALUES (?, ?, 0)";
                    SQLite::Statement stmt = SQLite::Statement::Create(deltaConn, sql);
                    stmt.Bind(1, dataRowid);
                    stmt.Bind(2, packageRowid);
                    stmt.Execute();
                }
            }

            // Record removed mappings (baseline but not current)
            for (const auto& val : baselineValues)
            {
                if (std::find(currentValues.begin(), currentValues.end(), val) == currentValues.end())
                {
                    // Find the rowid — it's in the baseline data table
                    SQLite::rowid_t dataRowid = GetBaselineDataTableRowid(baselineConn, std::string(tableName), valueName, val);
                    if (dataRowid != 0)
                    {
                        std::string sql = "INSERT OR IGNORE INTO " + deltaMapTable +
                            " (" + std::string(valueName) + ", package, is_removed) VALUES (?, ?, 1)";
                        SQLite::Statement stmt = SQLite::Statement::Create(deltaConn, sql);
                        stmt.Bind(1, dataRowid);
                        stmt.Bind(2, packageRowid);
                        stmt.Execute();
                    }
                }
            }
        }
    }

    Interface::Interface(Utility::NormalizationVersion normVersion) : m_normalizer(normVersion)
    {
    }

    SQLite::Version Interface::GetVersion() const
    {
        return { 2, 0 };
    }

    void Interface::CreateTables(SQLite::Connection& connection, CreateOptions options)
    {
        m_internalInterface = CreateInternalInterface();

        SQLite::Savepoint savepoint = SQLite::Savepoint::Create(connection, "createtables_v2_0");

        // We only create the internal tables at this point, the actual 2.0 tables are created in PrepareForPackaging
        m_internalInterface->CreateTables(connection, options);

        savepoint.Commit();

        m_internalInterfaceChecked = true;
    }

    SQLite::rowid_t Interface::AddManifest(SQLite::Connection& connection, const Manifest::Manifest& manifest, const std::optional<std::filesystem::path>& relativePath)
    {
        EnsureInternalInterface(connection, true);
        SQLite::rowid_t manifestId = m_internalInterface->AddManifest(connection, manifest, relativePath);
        PackageUpdateTrackingTable::Update(connection, m_internalInterface.get(), m_internalInterface->GetPropertyByPrimaryId(connection, manifestId, PackageVersionProperty::Id).value());
        return manifestId;
    }

    std::pair<bool, SQLite::rowid_t> Interface::UpdateManifest(SQLite::Connection& connection, const Manifest::Manifest& manifest, const std::optional<std::filesystem::path>& relativePath)
    {
        EnsureInternalInterface(connection, true);
        std::pair<bool, SQLite::rowid_t> result = m_internalInterface->UpdateManifest(connection, manifest, relativePath);
        if (result.first)
        {
            PackageUpdateTrackingTable::Update(connection, m_internalInterface.get(), m_internalInterface->GetPropertyByPrimaryId(connection, result.second, PackageVersionProperty::Id).value());
        }
        return result;
    }

    SQLite::rowid_t Interface::RemoveManifest(SQLite::Connection& connection, const Manifest::Manifest& manifest)
    {
        EnsureInternalInterface(connection, true);
        std::optional<SQLite::rowid_t> result = m_internalInterface->GetManifestIdByManifest(connection, manifest);

        // If the manifest doesn't actually exist, fail the remove.
        THROW_HR_IF(E_NOT_SET, !result);

        SQLite::rowid_t manifestId = result.value();
        RemoveManifestById(connection, manifestId);

        return manifestId;
    }

    void Interface::RemoveManifestById(SQLite::Connection& connection, SQLite::rowid_t manifestId)
    {
        EnsureInternalInterface(connection, true);
        std::optional<std::string> identifier = m_internalInterface->GetPropertyByPrimaryId(connection, manifestId, PackageVersionProperty::Id);
        m_internalInterface->RemoveManifestById(connection, manifestId);
        if (identifier)
        {
            PackageUpdateTrackingTable::Update(connection, m_internalInterface.get(), identifier.value());
        }
    }

    void Interface::PrepareForPackaging(SQLite::Connection&)
    {
        // We implement the context version
        THROW_HR(E_NOTIMPL);
    }

    void Interface::PrepareForPackaging(const SQLiteIndexContext& context)
    {
        EnsureInternalInterface(context.Connection, true);
        PrepareForPackaging(context, true);
    }

    bool Interface::CheckConsistency(const SQLite::Connection& connection, bool log) const
    {
        EnsureInternalInterface(connection);

        bool result = true;

#define AICLI_CHECK_CONSISTENCY(_check_) \
        if (result || log) \
        { \
            result = _check_ && result; \
        }

        if (m_internalInterface)
        {
            AICLI_CHECK_CONSISTENCY(m_internalInterface->CheckConsistency(connection, log));
            AICLI_CHECK_CONSISTENCY(PackageUpdateTrackingTable::CheckConsistency(connection, m_internalInterface.get(), log));

            return result;
        }

        AICLI_CHECK_CONSISTENCY((PackagesTable::CheckConsistency<
            PackagesTable::IdColumn,
            PackagesTable::NameColumn,
            PackagesTable::MonikerColumn,
            PackagesTable::LatestVersionColumn,
            PackagesTable::ARPMinVersionColumn,
            PackagesTable::ARPMaxVersionColumn>(connection, log)));

        // Check the 1:N map tables for consistency
        AICLI_CHECK_CONSISTENCY(TagsTable::CheckConsistency(connection, log));
        AICLI_CHECK_CONSISTENCY(CommandsTable::CheckConsistency(connection, log));

        AICLI_CHECK_CONSISTENCY(PackageFamilyNameTable::CheckConsistency(connection, log));
        AICLI_CHECK_CONSISTENCY(ProductCodeTable::CheckConsistency(connection, log));
        AICLI_CHECK_CONSISTENCY(NormalizedPackageNameTable::CheckConsistency(connection, log));
        AICLI_CHECK_CONSISTENCY(NormalizedPackagePublisherTable::CheckConsistency(connection, log));
        AICLI_CHECK_CONSISTENCY(UpgradeCodeTable::CheckConsistency(connection, log));

#undef AICLI_CHECK_CONSISTENCY

        return result;
    }

    ISQLiteIndex::SearchResult Interface::Search(const SQLite::Connection& connection, const SearchRequest& request) const
    {
        EnsureInternalInterface(connection);

        if (m_internalInterface)
        {
            return m_internalInterface->Search(connection, request);
        }

        SearchRequest requestCopy = request;
        return SearchInternal(connection, requestCopy);
    }

    std::optional<std::string> Interface::GetPropertyByPrimaryId(const SQLite::Connection& connection, SQLite::rowid_t primaryId, PackageVersionProperty property) const
    {
        EnsureInternalInterface(connection);

        if (m_internalInterface)
        {
            return m_internalInterface->GetPropertyByPrimaryId(connection, primaryId, property);
        }

        switch (property)
        {
        case PackageVersionProperty::Id:
            return PackagesTable::GetValueById<PackagesTable::IdColumn>(connection, primaryId);
        case PackageVersionProperty::Name:
            return PackagesTable::GetValueById<PackagesTable::NameColumn>(connection, primaryId);
        case PackageVersionProperty::Version:
            return PackagesTable::GetValueById<PackagesTable::LatestVersionColumn>(connection, primaryId);
        case PackageVersionProperty::Channel:
            return "";
        case PackageVersionProperty::ManifestSHA256Hash:
        {
            std::optional<SQLite::blob_t> hash = PackagesTable::GetValueById<PackagesTable::HashColumn>(connection, primaryId);
            return (!hash || hash->empty()) ? std::optional<std::string>{} : Utility::SHA256::ConvertToString(hash.value());
        }
        case PackageVersionProperty::ArpMinVersion:
            return PackagesTable::GetValueById<PackagesTable::ARPMinVersionColumn>(connection, primaryId);
        case PackageVersionProperty::ArpMaxVersion:
            return PackagesTable::GetValueById<PackagesTable::ARPMaxVersionColumn>(connection, primaryId);
        case PackageVersionProperty::Moniker:
            return PackagesTable::GetValueById<PackagesTable::MonikerColumn>(connection, primaryId);
        default:
            return {};
        }
    }

    std::vector<std::string> Interface::GetMultiPropertyByPrimaryId(const SQLite::Connection& connection, SQLite::rowid_t primaryId, PackageVersionMultiProperty property) const
    {
        EnsureInternalInterface(connection);

        if (m_internalInterface)
        {
            return m_internalInterface->GetMultiPropertyByPrimaryId(connection, primaryId, property);
        }

        switch (property)
        {
        case PackageVersionMultiProperty::PackageFamilyName:
            return PackageFamilyNameTable::GetValuesByPrimaryId(connection, primaryId);
        case PackageVersionMultiProperty::ProductCode:
            return ProductCodeTable::GetValuesByPrimaryId(connection, primaryId);
            // These values are not right, as they are normalized.  But they are good enough for now and all we have.
        case PackageVersionMultiProperty::Name:
            return NormalizedPackageNameTable::GetValuesByPrimaryId(connection, primaryId);
        case PackageVersionMultiProperty::Publisher:
            return NormalizedPackagePublisherTable::GetValuesByPrimaryId(connection, primaryId);
        case PackageVersionMultiProperty::UpgradeCode:
            return UpgradeCodeTable::GetValuesByPrimaryId(connection, primaryId);
        case PackageVersionMultiProperty::Tag:
            return TagsTable::GetValuesByPrimaryId(connection, primaryId);
        case PackageVersionMultiProperty::Command:
            return CommandsTable::GetValuesByPrimaryId(connection, primaryId);
        default:
            return {};
        }
    }

    std::optional<SQLite::rowid_t> Interface::GetManifestIdByKey(const SQLite::Connection& connection, SQLite::rowid_t id, std::string_view version, std::string_view channel) const
    {
        EnsureInternalInterface(connection);

        if (m_internalInterface)
        {
            return m_internalInterface->GetManifestIdByKey(connection, id, version, channel);
        }

        THROW_HR(E_NOT_VALID_STATE);
    }

    std::optional<SQLite::rowid_t> Interface::GetManifestIdByManifest(const SQLite::Connection& connection, const Manifest::Manifest& manifest) const
    {
        EnsureInternalInterface(connection);

        if (m_internalInterface)
        {
            return m_internalInterface->GetManifestIdByManifest(connection, manifest);
        }

        THROW_HR(E_NOT_VALID_STATE);
    }

    std::vector<ISQLiteIndex::VersionKey> Interface::GetVersionKeysById(const SQLite::Connection& connection, SQLite::rowid_t id) const
    {
        EnsureInternalInterface(connection);

        if (m_internalInterface)
        {
            return m_internalInterface->GetVersionKeysById(connection, id);
        }

        THROW_HR(E_NOT_VALID_STATE);
    }

    ISQLiteIndex::MetadataResult Interface::GetMetadataByManifestId(const SQLite::Connection&, SQLite::rowid_t) const
    {
        return {};
    }

    void Interface::SetMetadataByManifestId(SQLite::Connection&, SQLite::rowid_t, PackageVersionMetadata, std::string_view)
    {
    }

    Utility::NormalizedName Interface::NormalizeName(std::string_view name, std::string_view publisher) const
    {
        if (m_internalInterface)
        {
            return m_internalInterface->NormalizeName(name, publisher);
        }

        return m_normalizer.Normalize(name, publisher);
    }

    std::set<std::pair<SQLite::rowid_t, Utility::NormalizedString>> Interface::GetDependenciesByManifestRowId(const SQLite::Connection& connection, SQLite::rowid_t rowid) const
    {
        EnsureInternalInterface(connection);

        if (m_internalInterface)
        {
            return m_internalInterface->GetDependenciesByManifestRowId(connection, rowid);
        }

        THROW_HR(E_NOT_VALID_STATE);
    }

    std::vector<std::pair<SQLite::rowid_t, Utility::NormalizedString>> Interface::GetDependentsById(const SQLite::Connection& connection, AppInstaller::Manifest::string_t id) const
    {
        EnsureInternalInterface(connection);

        if (m_internalInterface)
        {
            return m_internalInterface->GetDependentsById(connection, id);
        }

        THROW_HR(E_NOT_VALID_STATE);
    }

    void Interface::DropTables(SQLite::Connection& connection)
    {
        EnsureInternalInterface(connection);

        if (m_internalInterface)
        {
            return m_internalInterface->DropTables(connection);
        }

        SQLite::Savepoint savepoint = SQLite::Savepoint::Create(connection, "drop_tables_v2_0");

        PackagesTable::Drop(connection);

        TagsTable::Drop(connection);
        CommandsTable::Drop(connection);

        PackageFamilyNameTable::Drop(connection);
        ProductCodeTable::Drop(connection);
        NormalizedPackageNameTable::Drop(connection);
        NormalizedPackagePublisherTable::Drop(connection);
        UpgradeCodeTable::Drop(connection);

        savepoint.Commit();
    }

    bool Interface::MigrateFrom(SQLite::Connection& connection, const ISQLiteIndex* current)
    {
        THROW_HR_IF_NULL(E_POINTER, current);

        auto currentVersion = current->GetVersion();
        if (currentVersion.MajorVersion != 1 || currentVersion.MinorVersion != 7)
        {
            return false;
        }

        SQLite::Savepoint savepoint = SQLite::Savepoint::Create(connection, "migrate_from_v2_0");

        // We only need to insert all of the existing packages into the update tracking table.
        PackageUpdateTrackingTable::EnsureExists(connection);
        SearchResult allPackages = current->Search(connection, {});

        for (const auto& packageMatch : allPackages.Matches)
        {
            std::vector<ISQLiteIndex::VersionKey> versionKeys = current->GetVersionKeysById(connection, packageMatch.first);
            ISQLiteIndex::VersionKey& latestVersionKey = versionKeys[0];
            PackageUpdateTrackingTable::Update(connection, current, current->GetPropertyByPrimaryId(connection, latestVersionKey.ManifestId, PackageVersionProperty::Id).value(), false);
        }

        savepoint.Commit();
        return true;
    }

    void Interface::SetProperty(SQLite::Connection& connection, Property property, const std::string& value)
    {
        switch (property)
        {
        case Property::PackageUpdateTrackingBaseTime:
        {
            int64_t baseTime = 0;
            if (value.empty())
            {
                baseTime = Utility::GetCurrentUnixEpoch();
            }
            else
            {
                baseTime = std::stoll(value);
            }
            SQLite::MetadataTable::SetNamedValue(connection, s_MetadataValueName_PackageUpdateTrackingBaseTime, std::to_string(baseTime));
        }
            break;

        default:
            THROW_WIN32(ERROR_NOT_SUPPORTED);
        }
    }

    std::unique_ptr<SearchResultsTable> Interface::CreateSearchResultsTable(const SQLite::Connection& connection) const
    {
        return std::make_unique<SearchResultsTable>(connection);
    }

    void Interface::PerformQuerySearch(SearchResultsTable& resultsTable, const RequestMatch& query) const
    {
        // First, do an exact match search for the folded system reference strings
        // We do this first because it is exact, and likely won't match anything else if it matches this.
        PackageMatchFilter filter(PackageMatchField::Unknown, MatchType::Exact, Utility::FoldCase(query.Value));

        for (PackageMatchField field : { PackageMatchField::PackageFamilyName, PackageMatchField::ProductCode, PackageMatchField::UpgradeCode })
        {
            filter.Field = field;
            resultsTable.SearchOnField(filter);
        }

        // Now search on the unfolded value
        filter.Value = query.Value;

        for (MatchType match : GetDefaultMatchTypeOrder(query.Type))
        {
            filter.Type = match;

            for (auto field : { PackageMatchField::Id, PackageMatchField::Name, PackageMatchField::Moniker, PackageMatchField::Command, PackageMatchField::Tag })
            {
                filter.Field = field;
                resultsTable.SearchOnField(filter);
            }
        }
    }

    OneToManyTableSchema Interface::GetOneToManyTableSchema() const
    {
        return OneToManyTableSchema::Version_2_0;
    }

    ISQLiteIndex::SearchResult Interface::SearchInternal(const SQLite::Connection& connection, SearchRequest& request) const
    {
        anon::FoldPackageMatchFilters(request.Inclusions);
        anon::FoldPackageMatchFilters(request.Filters);

        if (request.Purpose == SearchPurpose::CorrelationToInstalled)
        {
            // Correlate from available package to installed package
            // For available package to installed package mapping, only one try is needed.
            // For example, if ARP DisplayName contains arch, then the installed package's ARP DisplayName should also include arch.
            auto candidateInclusionsWithArch = request.Inclusions;
            if (anon::UpdatePackageMatchFilters(candidateInclusionsWithArch, m_normalizer, Utility::NormalizationField::Architecture))
            {
                // If DisplayNames contain arch, only use Inclusions with arch for search
                request.Inclusions = candidateInclusionsWithArch;
            }
            else
            {
                // Otherwise, just update the Inclusions with normalization
                anon::UpdatePackageMatchFilters(request.Inclusions, m_normalizer);
            }

            return BasicSearchInternal(connection, request);
        }
        else if (request.Purpose == SearchPurpose::CorrelationToAvailable)
        {
            // For installed package to available package correlation,
            // try the search with NormalizedName with Arch first, if not found, try with all values.
            // This can be extended in the future for more granular search requests.
            std::vector<SearchRequest> candidateSearches;
            auto candidateSearchWithArch = request;
            if (anon::UpdatePackageMatchFilters(candidateSearchWithArch.Inclusions, m_normalizer, Utility::NormalizationField::Architecture))
            {
                candidateSearches.emplace_back(std::move(candidateSearchWithArch));
            }
            anon::UpdatePackageMatchFilters(request.Inclusions, m_normalizer);
            candidateSearches.emplace_back(request);

            SearchResult result;
            for (auto& candidateSearch : candidateSearches)
            {
                result = BasicSearchInternal(connection, candidateSearch);
                if (!result.Matches.empty())
                {
                    break;
                }
            }

            return result;
        }
        else
        {
            anon::UpdatePackageMatchFilters(request.Inclusions, m_normalizer);
            anon::UpdatePackageMatchFilters(request.Filters, m_normalizer);

            return BasicSearchInternal(connection, request);
        }
    }

    ISQLiteIndex::SearchResult Interface::BasicSearchInternal(const SQLite::Connection& connection, const SearchRequest& request) const
    {
        if (request.IsForEverything())
        {
            std::vector<SQLite::rowid_t> ids = PackagesTable::GetAllRowIds(connection, PackagesTable::IdColumn::Name, request.MaximumResults);

            SearchResult result;
            for (SQLite::rowid_t id : ids)
            {
                result.Matches.emplace_back(std::make_pair(id, PackageMatchFilter(PackageMatchField::Id, MatchType::Wildcard)));
            }

            result.Truncated = (request.MaximumResults && PackagesTable::GetCount(connection) > request.MaximumResults);

            return result;
        }

        // First phase, create the search results table and populate it with the initial results.
        // If the Query is provided, we search across many fields and put results in together.
        // If Inclusions has fields, we add these to the data.
        // If neither is defined, we take the first filter and use it as the initial results search.
        std::unique_ptr<SearchResultsTable> resultsTable = CreateSearchResultsTable(connection);
        bool inclusionsAttempted = false;

        if (request.Query)
        {
            // Perform searches across multiple tables to populate the initial results.
            PerformQuerySearch(*resultsTable.get(), request.Query.value());

            inclusionsAttempted = true;
        }

        if (!request.Inclusions.empty())
        {
            for (auto include : request.Inclusions)
            {
                for (MatchType match : GetDefaultMatchTypeOrder(include.Type))
                {
                    include.Type = match;
                    resultsTable->SearchOnField(include);
                }
            }

            inclusionsAttempted = true;
        }

        size_t filterIndex = 0;
        if (!inclusionsAttempted)
        {
            THROW_HR_IF(E_UNEXPECTED, request.Filters.empty());

            // Perform search for just the field matching the first filter
            PackageMatchFilter filter = request.Filters[0];

            for (MatchType match : GetDefaultMatchTypeOrder(filter.Type))
            {
                filter.Type = match;
                resultsTable->SearchOnField(filter);
            }

            // Skip the filter as we already know everything matches
            filterIndex = 1;
        }

        // Remove any duplicate manifest entries
        resultsTable->RemoveDuplicatePackageRows();

        // Second phase, for remaining filters, flag matching search results, then remove unflagged values.
        for (size_t i = filterIndex; i < request.Filters.size(); ++i)
        {
            PackageMatchFilter filter = request.Filters[i];

            resultsTable->PrepareToFilter();

            for (MatchType match : GetDefaultMatchTypeOrder(filter.Type))
            {
                filter.Type = match;
                resultsTable->FilterOnField(filter);
            }

            resultsTable->CompleteFilter();
        }

        return resultsTable->GetSearchResults(request.MaximumResults);
    }

    void Interface::PrepareForPackaging(const SQLiteIndexContext& context, bool vacuum)
    {
        SQLite::Connection& connection = context.Connection;

        // TODO: We may need to set the baseline time to the max update tracking time +1 to only catch new incoming changes
        //       This assumes some delay between delta generation and the next package update.
        // TODO: We also need to ensure that our times are UTC / not impacted by timezone shifts, etc.
        SQLite::MetadataTable::SetNamedValue(connection, s_MetadataValueName_DeltaBaselineTime, std::to_string(Utility::GetCurrentUnixEpoch()));

        // Get the base time from metadata
        int64_t updateBaseTime = 0;
        std::optional<std::string> updateBaseTimeString = SQLite::MetadataTable::TryGetNamedValue<std::string>(connection, s_MetadataValueName_PackageUpdateTrackingBaseTime);
        if (updateBaseTimeString && !updateBaseTimeString->empty())
        {
            updateBaseTime = std::stoll(updateBaseTimeString.value());
        }

        // Get the output directory or use the file path
        std::filesystem::path baseOutputDirectory;

        if (context.Data.Contains(Property::IntermediateFileOutputPath))
        {
            baseOutputDirectory = context.Data.Get<Property::IntermediateFileOutputPath>();
        }
        else if (context.Data.Contains(Property::DatabaseFilePath))
        {
            baseOutputDirectory = context.Data.Get<Property::DatabaseFilePath>();
            baseOutputDirectory = baseOutputDirectory.parent_path();
        }

        THROW_WIN32_IF(ERROR_INVALID_STATE, baseOutputDirectory.empty() || baseOutputDirectory.is_relative());

        // TEMP
        PackageUpdateTrackingTable::EnsureExists(connection);

        // Output all of the changed package version manifests since the base time to the target location
        for (const auto& packageData : PackageUpdateTrackingTable::GetUpdatesSince(connection, updateBaseTime))
        {
            if (packageData.IsRemoved)
            {
                continue;
            }

            std::filesystem::path packageDirectory = baseOutputDirectory /
                Manifest::PackageVersionDataManifest::GetRelativeDirectoryPath(packageData.PackageIdentifier, Utility::SHA256::ConvertToString(packageData.Hash));

            std::filesystem::create_directories(packageDirectory);

            std::filesystem::path manifestPath = packageDirectory / Manifest::PackageVersionDataManifest::VersionManifestCompressedFileName();

            AICLI_LOG(Repo, Info, << "Writing PackageVersionDataManifest for [" << packageData.PackageIdentifier << "] to [" << manifestPath << "]");

            std::ofstream stream(manifestPath, std::ios_base::out | std::ios_base::binary | std::ios_base::trunc);
            THROW_LAST_ERROR_IF(stream.fail());
            stream.write(reinterpret_cast<const char*>(packageData.Manifest.data()), packageData.Manifest.size());
            THROW_LAST_ERROR_IF(stream.fail());
            stream.flush();
        }

        SQLite::Savepoint savepoint = SQLite::Savepoint::Create(connection, "prepareforpackaging_v2_0");

        // Create the 2.0 data tables
        PackagesTable::Create<
            PackagesTable::IdColumn,
            PackagesTable::NameColumn,
            PackagesTable::MonikerColumn,
            PackagesTable::LatestVersionColumn,
            PackagesTable::ARPMinVersionColumn,
            PackagesTable::ARPMaxVersionColumn,
            PackagesTable::HashColumn
        >(connection);

        TagsTable::Create(connection, GetOneToManyTableSchema());
        CommandsTable::Create(connection, GetOneToManyTableSchema());

        PackageFamilyNameTable::Create(connection);
        ProductCodeTable::Create(connection);
        NormalizedPackageNameTable::Create(connection);
        NormalizedPackagePublisherTable::Create(connection);
        UpgradeCodeTable::Create(connection);

        // Copy data from 1.7 tables to 2.0 tables
        SearchResult allPackages = m_internalInterface->Search(connection, {});

        for (const auto& packageMatch : allPackages.Matches)
        {
            std::vector<ISQLiteIndex::VersionKey> versionKeys = m_internalInterface->GetVersionKeysById(connection, packageMatch.first);
            ISQLiteIndex::VersionKey& latestVersionKey = versionKeys[0];

            std::string packageIdentifier = m_internalInterface->GetPropertyByPrimaryId(connection, latestVersionKey.ManifestId, PackageVersionProperty::Id).value();

            std::vector<PackagesTable::NameValuePair> packageData{
                { PackagesTable::IdColumn::Name, packageIdentifier },
                { PackagesTable::NameColumn::Name, m_internalInterface->GetPropertyByPrimaryId(connection, latestVersionKey.ManifestId, PackageVersionProperty::Name).value() },
                { PackagesTable::LatestVersionColumn::Name, latestVersionKey.VersionAndChannel.GetVersion().ToString() },
            };

            auto addIfPresent = [&](std::string_view name, std::optional<std::string>&& value)
                {
                    if (value && !value->empty())
                    {
                        packageData.emplace_back(PackagesTable::NameValuePair{ name, std::move(value).value() });
                    }
                };

            addIfPresent(PackagesTable::MonikerColumn::Name, m_internalInterface->GetPropertyByPrimaryId(connection, latestVersionKey.ManifestId, PackageVersionProperty::Moniker).value());
            addIfPresent(PackagesTable::ARPMinVersionColumn::Name, m_internalInterface->GetPropertyByPrimaryId(connection, latestVersionKey.ManifestId, PackageVersionProperty::ArpMinVersion).value());
            addIfPresent(PackagesTable::ARPMaxVersionColumn::Name, m_internalInterface->GetPropertyByPrimaryId(connection, latestVersionKey.ManifestId, PackageVersionProperty::ArpMaxVersion).value());

            auto idRowId = V1_0::IdTable::SelectIdByValue(connection, packageIdentifier);
            THROW_HR_IF(E_NOT_VALID_STATE, !idRowId);

            SQLite::rowid_t packageId = PackagesTable::InsertWithRowId(connection, idRowId.value(), packageData);

            PackagesTable::UpdateValueIdById<PackagesTable::HashColumn>(connection, packageId, PackageUpdateTrackingTable::GetDataHash(connection, packageIdentifier));

            for (const auto& versionKey : versionKeys)
            {
                TagsTable::EnsureExistsAndInsert(connection, m_internalInterface->GetMultiPropertyByPrimaryId(connection, versionKey.ManifestId, PackageVersionMultiProperty::Tag), packageId);
                CommandsTable::EnsureExistsAndInsert(connection, m_internalInterface->GetMultiPropertyByPrimaryId(connection, versionKey.ManifestId, PackageVersionMultiProperty::Command), packageId);

                PackageFamilyNameTable::EnsureExists(connection, m_internalInterface->GetMultiPropertyByPrimaryId(connection, versionKey.ManifestId, PackageVersionMultiProperty::PackageFamilyName), packageId);
                ProductCodeTable::EnsureExists(connection, m_internalInterface->GetMultiPropertyByPrimaryId(connection, versionKey.ManifestId, PackageVersionMultiProperty::ProductCode), packageId);
                NormalizedPackageNameTable::EnsureExists(connection, m_internalInterface->GetMultiPropertyByPrimaryId(connection, versionKey.ManifestId, PackageVersionMultiProperty::Name), packageId);
                NormalizedPackagePublisherTable::EnsureExists(connection, m_internalInterface->GetMultiPropertyByPrimaryId(connection, versionKey.ManifestId, PackageVersionMultiProperty::Publisher), packageId);
                UpgradeCodeTable::EnsureExists(connection, m_internalInterface->GetMultiPropertyByPrimaryId(connection, versionKey.ManifestId, PackageVersionMultiProperty::UpgradeCode), packageId);
            }
        }

        // Generate the delta index before dropping the tracking table (which is needed for delta construction).
        // Delta generation is triggered by setting DeltaBaselineIndexPath and DeltaOutputPath on the context.
        if (context.Data.Contains(Property::DeltaBaselineIndexPath) &&
            context.Data.Contains(Property::DeltaOutputPath))
        {
            // Delta packaging requires schema 2.1+ (is_removed column in update_tracking).
            THROW_WIN32_IF(ERROR_NOT_SUPPORTED, GetVersion().MinorVersion < 1);

            std::filesystem::path baselinePath = context.Data.Get<Property::DeltaBaselineIndexPath>();
            std::filesystem::path deltaOutputPath = context.Data.Get<Property::DeltaOutputPath>();

            AICLI_LOG(Repo, Info, << "Generating delta index at [" << deltaOutputPath << "] against baseline [" << baselinePath << "]");

            SQLite::Connection baselineConn = SQLite::Connection::Create(baselinePath.u8string(), SQLite::Connection::OpenDisposition::ReadOnly);

            int64_t deltaUpdateBaseTime = 0;
            std::optional<std::string> deltaUpdateBaseTimeString = SQLite::MetadataTable::TryGetNamedValue<std::string>(baselineConn, s_MetadataValueName_DeltaBaselineTime);
            if (deltaUpdateBaseTimeString && !deltaUpdateBaseTimeString->empty())
            {
                deltaUpdateBaseTime = std::stoll(deltaUpdateBaseTimeString.value());
            }

            auto changedPackages = PackageUpdateTrackingTable::GetUpdatesSince(connection, deltaUpdateBaseTime);
            if (changedPackages.empty())
            {
                AICLI_LOG(Repo, Info, << "No changed packages found; skipping delta generation");
            }
            else
            {
                SQLite::Connection deltaConn = SQLite::Connection::Create(
                    deltaOutputPath.u8string(), SQLite::Connection::OpenDisposition::Create);

                anon::CreateDeltaSchema(deltaConn);

                SQLite::rowid_t maxBaselinePackageRowid = anon::GetMaxPackageRowid(baselineConn);
                SQLite::rowid_t nextNewPackageRowid = maxBaselinePackageRowid;

                SQLite::rowid_t maxBaselineTagsRowid = anon::GetMaxDataTableRowid(baselineConn, "tags2");
                SQLite::rowid_t nextNewTagsRowid = maxBaselineTagsRowid;

                SQLite::rowid_t maxBaselineCommandsRowid = anon::GetMaxDataTableRowid(baselineConn, "commands2");
                SQLite::rowid_t nextNewCommandsRowid = maxBaselineCommandsRowid;

                SQLite::Savepoint deltaSavepoint = SQLite::Savepoint::Create(deltaConn, "delta_build");

                for (const auto& pkg : changedPackages)
                {
                    SQLite::rowid_t packageRowid = anon::GetBaselinePackageRowid(baselineConn, pkg.PackageIdentifier);

                    if (pkg.IsRemoved)
                    {
                        if (packageRowid == 0)
                        {
                            // Package was added and removed within the same tracking window; skip.
                            continue;
                        }

                        AICLI_LOG(Repo, Verbose, << "Delta: recording removal of [" << pkg.PackageIdentifier << "] (rowid=" << packageRowid << ")");

                        std::string sql = "INSERT OR REPLACE INTO delta_packages (rowid, id, name, latest_version, is_removed) VALUES (?, ?, '', '', 1)";
                        SQLite::Statement stmt = SQLite::Statement::Create(deltaConn, sql);
                        stmt.Bind(1, packageRowid);
                        stmt.Bind(2, pkg.PackageIdentifier);
                        stmt.Execute();
                    }
                    else
                    {
                        bool isNewPackage = (packageRowid == 0);
                        if (isNewPackage)
                        {
                            packageRowid = ++nextNewPackageRowid;
                        }

                        AICLI_LOG(Repo, Verbose, << "Delta: recording " << (isNewPackage ? "addition" : "update") << " of [" << pkg.PackageIdentifier << "] (rowid=" << packageRowid << ")");

                        {
                            std::string sql = "SELECT id, name, moniker, latest_version, arp_min_version, arp_max_version, hash "
                                              "FROM packages WHERE id = ?";
                            SQLite::Statement stmt = SQLite::Statement::Create(connection, sql);
                            stmt.Bind(1, pkg.PackageIdentifier);
                            THROW_HR_IF(E_NOT_SET, !stmt.Step());

                            std::string id = stmt.GetColumn<std::string>(0);
                            std::string name = stmt.GetColumn<std::string>(1);
                            std::string moniker = stmt.GetColumnIsNull(2) ? "" : stmt.GetColumn<std::string>(2);
                            std::string latestVersion = stmt.GetColumn<std::string>(3);
                            std::string arpMin = stmt.GetColumnIsNull(4) ? "" : stmt.GetColumn<std::string>(4);
                            std::string arpMax = stmt.GetColumnIsNull(5) ? "" : stmt.GetColumn<std::string>(5);
                            SQLite::blob_t hash = stmt.GetColumnIsNull(6) ? SQLite::blob_t{} : stmt.GetColumn<SQLite::blob_t>(6);

                            std::string insertSql =
                                "INSERT OR REPLACE INTO delta_packages "
                                "(rowid, id, name, moniker, latest_version, arp_min_version, arp_max_version, hash, is_removed) "
                                "VALUES (?, ?, ?, ?, ?, ?, ?, ?, 0)";
                            SQLite::Statement insertStmt = SQLite::Statement::Create(deltaConn, insertSql);
                            insertStmt.Bind(1, packageRowid);
                            insertStmt.Bind(2, id);
                            insertStmt.Bind(3, name);
                            if (moniker.empty()) insertStmt.Bind(4, nullptr); else insertStmt.Bind(4, moniker);
                            insertStmt.Bind(5, latestVersion);
                            if (arpMin.empty()) insertStmt.Bind(6, nullptr); else insertStmt.Bind(6, arpMin);
                            if (arpMax.empty()) insertStmt.Bind(7, nullptr); else insertStmt.Bind(7, arpMax);
                            if (hash.empty()) insertStmt.Bind(8, nullptr); else insertStmt.Bind(8, hash);
                            insertStmt.Execute();
                        }

                        static constexpr std::pair<std::string_view, std::string_view> s_DeltaSysRefTables[] = {
                            { "pfns2",            "pfn" },
                            { "productcodes2",    "productcode" },
                            { "norm_names2",      "norm_name" },
                            { "norm_publishers2", "norm_publisher" },
                            { "upgradecodes2",    "upgradecode" },
                        };

                        for (const auto& [table, value] : s_DeltaSysRefTables)
                        {
                            anon::ProcessDeltaSysRefTable(deltaConn, connection, baselineConn,
                                table, value, packageRowid, pkg.PackageIdentifier);
                        }

                        anon::ProcessDeltaOneToManyTable(deltaConn, connection, baselineConn,
                            "tags2", "tag", packageRowid, nextNewTagsRowid);
                        anon::ProcessDeltaOneToManyTable(deltaConn, connection, baselineConn,
                            "commands2", "command", packageRowid, nextNewCommandsRowid);
                    }
                }

                deltaSavepoint.Commit();

                AICLI_LOG(Repo, Info, << "Delta index generation complete");
            }
        }

        PackagesTable::PrepareForPackaging<
            PackagesTable::IdColumn,
            PackagesTable::NameColumn,
            PackagesTable::MonikerColumn,
            PackagesTable::LatestVersionColumn,
            PackagesTable::ARPMinVersionColumn,
            PackagesTable::ARPMaxVersionColumn,
            PackagesTable::HashColumn
        >(connection);

        TagsTable::PrepareForPackaging(connection);
        CommandsTable::PrepareForPackaging(connection);

        PackageUpdateTrackingTable::Drop(connection);

        // The tables based on SystemReferenceStringTable don't need a prepare currently

        // Drop 1.7 tables
        m_internalInterface->DropTables(connection);

        savepoint.Commit();

        m_internalInterface.reset();

        if (vacuum)
        {
            Vacuum(connection);
        }
    }

    void Interface::Vacuum(const SQLite::Connection& connection)
    {
        SQLite::Builder::StatementBuilder builder;
        builder.Vacuum();
        builder.Execute(connection);
    }

    void Interface::EnsureInternalInterface(const SQLite::Connection& connection, bool requireInternalInterface) const
    {
        if (!m_internalInterfaceChecked)
        {
            // In delta read mode the TEMP VIEWs are already set up; no internal interface needed.
            if (!m_isDeltaReadMode && !PackagesTable::Exists(connection))
            {
                m_internalInterface = CreateInternalInterface();
            }

            m_internalInterfaceChecked = true;
        }

        THROW_HR_IF(E_NOT_VALID_STATE, requireInternalInterface && !m_internalInterface);
    }

    std::unique_ptr<Schema::ISQLiteIndex> Interface::CreateInternalInterface() const
    {
        return CreateISQLiteIndex({ 1, 7 });
    }

    void Interface::SetupDeltaReadMode(SQLite::Connection& connection, const std::filesystem::path& baselinePath)
    {
        AICLI_LOG(Repo, Info, << "Setting up delta read mode with baseline [" << baselinePath << "]");

        // Attach the baseline database under the "baseline" schema name
        {
            std::string attachSql = "ATTACH DATABASE ? AS baseline";
            SQLite::Statement stmt = SQLite::Statement::Create(connection, attachSql);
            stmt.Bind(1, baselinePath.u8string());
            stmt.Execute();
        }

        // TEMP VIEW: packages
        // Delta entries (added/updated) override baseline; removed packages are excluded.
        anon::ExecuteSQL(connection, R"(
            CREATE TEMP VIEW packages AS
            SELECT rowid, id, name, moniker, latest_version, arp_min_version, arp_max_version, hash
            FROM delta_packages WHERE is_removed = 0
            UNION ALL
            SELECT p.rowid, p.id, p.name, p.moniker, p.latest_version, p.arp_min_version, p.arp_max_version, p.hash
            FROM baseline.packages p
            WHERE p.id NOT IN (SELECT id FROM delta_packages)
        )");

        // TEMP VIEWs: SystemReference tables (pfns2, productcodes2, norm_names2, norm_publishers2, upgradecodes2)
        // For changed packages: delta has the full current set (is_removed=0 = current, is_removed=1 = removed).
        // For unchanged packages: baseline rows pass through.
        static constexpr std::pair<std::string_view, std::string_view> s_SysRefTables[] = {
            { "pfns2",            "pfn" },
            { "productcodes2",    "productcode" },
            { "norm_names2",      "norm_name" },
            { "norm_publishers2", "norm_publisher" },
            { "upgradecodes2",    "upgradecode" },
        };
        for (const auto& [table, value] : s_SysRefTables)
        {
            std::string sql =
                "CREATE TEMP VIEW " + std::string(table) + " AS "
                "SELECT " + std::string(value) + ", package FROM delta_" + std::string(table) + " WHERE is_removed = 0 "
                "UNION ALL "
                "SELECT " + std::string(value) + ", package FROM baseline." + std::string(table) + " "
                "WHERE package NOT IN (SELECT package FROM delta_" + std::string(table) + ")";
            anon::ExecuteSQL(connection, sql);
        }

        // TEMP VIEW: tags2 / commands2 data tables (rowid + value).
        // Delta only contains NEW strings (with rowids > baseline max); no conflicts possible.
        static constexpr std::pair<std::string_view, std::string_view> s_OneToManyTables[] = {
            { "tags2",     "tag" },
            { "commands2", "command" },
        };
        for (const auto& [table, value] : s_OneToManyTables)
        {
            std::string sql =
                "CREATE TEMP VIEW " + std::string(table) + " AS "
                "SELECT rowid, " + std::string(value) + " FROM delta_" + std::string(table) + " "
                "UNION ALL "
                "SELECT rowid, " + std::string(value) + " FROM baseline." + std::string(table);
            anon::ExecuteSQL(connection, sql);

            // TEMP VIEW: tags2_map / commands2_map
            // For changed packages: delta has the full current set of mappings.
            // For unchanged packages: baseline mappings pass through.
            std::string mapSql =
                "CREATE TEMP VIEW " + std::string(table) + "_map AS "
                "SELECT " + std::string(value) + ", package FROM delta_" + std::string(table) + "_map WHERE is_removed = 0 "
                "UNION ALL "
                "SELECT bm." + std::string(value) + ", bm.package "
                "FROM baseline." + std::string(table) + "_map bm "
                "WHERE bm.package NOT IN (SELECT package FROM delta_" + std::string(table) + "_map)";
            anon::ExecuteSQL(connection, mapSql);
        }

        m_isDeltaReadMode = true;
        m_internalInterfaceChecked = true; // Suppress normal EnsureInternalInterface logic
    }
}
