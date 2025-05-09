// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#include "pch.h"
#include "TestCommon.h"
#include <AppInstallerErrors.h>
#include <winget/SQLiteWrapper.h>
#include <winget/SQLiteStatementBuilder.h>

using namespace AppInstaller::SQLite;
using namespace std::string_literals;

static const char* s_firstColumn = "first";
static const char* s_secondColumn = "second";
static const char* s_tableName = "simpletest";
static const char* s_savepoint = "simplesave";

static const char* s_CreateSimpleTestTableSQL = R"(
CREATE TABLE [main].[simpletest](
  [first] INT, 
  [second] TEXT);
)";

static const char* s_insertToSimpleTestTableSQL = R"(
insert into simpletest (first, second) values (?, ?)
)";

static const char* s_selectFromSimpleTestTableSQL = R"(
select first, second from simpletest
)";

void CreateSimpleTestTable(Connection& connection)
{
    Builder::StatementBuilder builder;
    builder.CreateTable(s_tableName).Columns({
        Builder::ColumnBuilder(s_firstColumn, Builder::Type::Int),
        Builder::ColumnBuilder(s_secondColumn, Builder::Type::Text),
        });

    Statement createTable = builder.Prepare(connection);
    REQUIRE_FALSE(createTable.Step());
    REQUIRE(createTable.GetState() == Statement::State::Completed);
}

void InsertIntoSimpleTestTable(Connection& connection, int firstVal, const std::string& secondVal)
{
    Builder::StatementBuilder builder;
    builder.InsertInto(s_tableName).Columns({ s_firstColumn, s_secondColumn }).Values(firstVal, secondVal);
    Statement insert = builder.Prepare(connection);

    REQUIRE_FALSE(insert.Step());
    REQUIRE(insert.GetState() == Statement::State::Completed);
}

void UpdateSimpleTestTable(Connection& connection, int firstVal, const std::string& secondVal)
{
    Builder::StatementBuilder update;
    update.Update(s_tableName).Set().Column(s_firstColumn).Equals(firstVal).Column(s_secondColumn).Equals(secondVal);
    update.Execute(connection);
}

void InsertIntoSimpleTestTableWithNull(Connection& connection, int firstVal)
{
    Builder::StatementBuilder builder;
    builder.InsertInto(s_tableName).Columns({ s_firstColumn, s_secondColumn }).Values(firstVal, nullptr);
    Statement insert = builder.Prepare(connection);

    REQUIRE_FALSE(insert.Step());
    REQUIRE(insert.GetState() == Statement::State::Completed);
}

void SelectFromSimpleTestTableOnlyOneRow(Connection& connection, int firstVal, const std::string& secondVal)
{
    Builder::StatementBuilder builder;
    builder.Select({ s_firstColumn, s_secondColumn }).From(s_tableName);
    Statement select = builder.Prepare(connection);

    REQUIRE(select.Step());
    REQUIRE(select.GetState() == Statement::State::HasRow);

    int firstRead = select.GetColumn<int>(0);
    std::string secondRead = select.GetColumn<std::string>(1);

    REQUIRE(firstVal == firstRead);
    REQUIRE(secondVal == secondRead);

    auto tuple = select.GetRow<int, std::string>();

    REQUIRE(firstVal == std::get<0>(tuple));
    REQUIRE(secondVal == std::get<1>(tuple));

    REQUIRE_FALSE(select.Step());
    REQUIRE(select.GetState() == Statement::State::Completed);

    select.Reset();
    REQUIRE(select.GetState() == Statement::State::Prepared);

    REQUIRE(select.Step());
    REQUIRE(select.GetState() == Statement::State::HasRow);
}

TEST_CASE("SQLiteWrapperMemoryCreate", "[sqlitewrapper]")
{
    Connection connection = Connection::Create(SQLITE_MEMORY_DB_CONNECTION_TARGET, Connection::OpenDisposition::Create);

    CreateSimpleTestTable(connection);

    int firstVal = 1;
    std::string secondVal = "test";

    InsertIntoSimpleTestTable(connection, firstVal, secondVal);

    SelectFromSimpleTestTableOnlyOneRow(connection, firstVal, secondVal);
}

TEST_CASE("SQLiteWrapperFileCreateAndReopen", "[sqlitewrapper]")
{
    TestCommon::TempFile tempFile{ "repolibtest_tempdb"s, ".db"s };
    INFO("Using temporary file named: " << tempFile.GetPath());

    int firstVal = 1;
    std::string secondVal = "test";

    // Create the DB and some data
    {
        Connection connection = Connection::Create(tempFile, Connection::OpenDisposition::Create);

        CreateSimpleTestTable(connection);

        InsertIntoSimpleTestTable(connection, firstVal, secondVal);
    }

    // Reopen the DB and read data
    {
        Connection connection = Connection::Create(tempFile, Connection::OpenDisposition::ReadWrite);

        SelectFromSimpleTestTableOnlyOneRow(connection, firstVal, secondVal);
    }
}

TEST_CASE("SQLiteWrapperSavepointRollback", "[sqlitewrapper]")
{
    Connection connection = Connection::Create(SQLITE_MEMORY_DB_CONNECTION_TARGET, Connection::OpenDisposition::Create);

    int firstVal = 1;
    std::string secondVal = "test";

    CreateSimpleTestTable(connection);

    Savepoint savepoint = Savepoint::Create(connection, "test_savepoint");

    InsertIntoSimpleTestTable(connection, firstVal, secondVal);

    savepoint.Rollback();

    Statement select = Statement::Create(connection, s_selectFromSimpleTestTableSQL);
    REQUIRE(!select.Step());
    REQUIRE(select.GetState() == Statement::State::Completed);
}

TEST_CASE("SQLiteWrapperSavepointRollbackOnDestruct", "[sqlitewrapper]")
{
    Connection connection = Connection::Create(SQLITE_MEMORY_DB_CONNECTION_TARGET, Connection::OpenDisposition::Create);

    int firstVal = 1;
    std::string secondVal = "test";

    CreateSimpleTestTable(connection);

    {
        Savepoint savepoint = Savepoint::Create(connection, "test_savepoint");

        InsertIntoSimpleTestTable(connection, firstVal, secondVal);
    }

    Statement select = Statement::Create(connection, s_selectFromSimpleTestTableSQL);
    REQUIRE(!select.Step());
    REQUIRE(select.GetState() == Statement::State::Completed);
}

TEST_CASE("SQLiteWrapperSavepointCommit", "[sqlitewrapper]")
{
    Connection connection = Connection::Create(SQLITE_MEMORY_DB_CONNECTION_TARGET, Connection::OpenDisposition::Create);

    int firstVal = 1;
    std::string secondVal = "test";

    CreateSimpleTestTable(connection);

    {
        Savepoint savepoint = Savepoint::Create(connection, "test_savepoint");

        InsertIntoSimpleTestTable(connection, firstVal, secondVal);

        savepoint.Commit();
    }

    SelectFromSimpleTestTableOnlyOneRow(connection, firstVal, secondVal);
}

TEST_CASE("SQLiteWrapperSavepointReuse", "[sqlitewrapper]")
{
    TestCommon::TempFile tempFile{ "repolibtest_tempdb"s, ".db"s };
    INFO("Using temporary file named: " << tempFile.GetPath());

    int firstVal = 1;
    std::string secondVal = "test";

    // Create the DB and some data
    {
        Connection connection = Connection::Create(tempFile, Connection::OpenDisposition::Create);

        CreateSimpleTestTable(connection);

        InsertIntoSimpleTestTable(connection, firstVal, secondVal);
    }

    // Reopen the DB and update with a single savepoint
    {
        Connection connection = Connection::Create(tempFile, Connection::OpenDisposition::ReadWrite);

        Savepoint savepoint = Savepoint::Create(connection, s_savepoint);

        firstVal = 2;
        secondVal = "test2";
        UpdateSimpleTestTable(connection, firstVal, secondVal);
        
        savepoint.Commit();
    }

    {
        Connection connection = Connection::Create(tempFile, Connection::OpenDisposition::ReadWrite);
        SelectFromSimpleTestTableOnlyOneRow(connection, firstVal, secondVal);
    }

    // Reopen the DB and update with a multiple savepoint
    {
        Connection connection = Connection::Create(tempFile, Connection::OpenDisposition::ReadWrite);

        {
            Savepoint savepoint = Savepoint::Create(connection, s_savepoint);

            firstVal = 3;
            secondVal = "test3";
            UpdateSimpleTestTable(connection, firstVal, secondVal);
        }

        {
            Savepoint savepoint = Savepoint::Create(connection, s_savepoint);

            firstVal = 4;
            secondVal = "test4";
            UpdateSimpleTestTable(connection, firstVal, secondVal);

            savepoint.Commit();
        }
    }

    {
        Connection connection = Connection::Create(tempFile, Connection::OpenDisposition::ReadWrite);
        SelectFromSimpleTestTableOnlyOneRow(connection, firstVal, secondVal);
    }
}

TEST_CASE("SQLiteWrapper_EscapeStringForLike", "[sqlitewrapper]")
{
    std::string escape(EscapeCharForLike);

    std::string input = "test";
    std::string output = EscapeStringForLike(input);
    REQUIRE(input == output);

    input = EscapeCharForLike;
    output = EscapeStringForLike(input);
    REQUIRE((input + input) == output);

    input = "%";
    output = EscapeStringForLike(input);
    REQUIRE((escape + input) == output);

    input = "_";
    output = EscapeStringForLike(input);
    REQUIRE((escape + input) == output);

    input = "%_A_%";
    std::string expected = escape + "%" + escape + "_A" + escape + "_" + escape + "%";
    output = EscapeStringForLike(input);
    REQUIRE(expected == output);
}

TEST_CASE("SQLiteWrapper_BindWithEmbeddedNull", "[sqlitewrapper]")
{
    Connection connection = Connection::Create(SQLITE_MEMORY_DB_CONNECTION_TARGET, Connection::OpenDisposition::Create);

    CreateSimpleTestTable(connection);

    int firstVal = 1;
    std::string secondVal = "test";
    secondVal[1] = '\0';

    REQUIRE_THROWS_HR(InsertIntoSimpleTestTable(connection, firstVal, secondVal), APPINSTALLER_CLI_ERROR_BIND_WITH_EMBEDDED_NULL);
}

TEST_CASE("SQLiteWrapper_PrepareFailure", "[sqlitewrapper]")
{
    Connection connection = Connection::Create(SQLITE_MEMORY_DB_CONNECTION_TARGET, Connection::OpenDisposition::Create);

    CreateSimpleTestTable(connection);

    Builder::StatementBuilder builder;
    builder.Select({ s_firstColumn, s_secondColumn }).From(std::string{ s_tableName } + "2").Where(s_firstColumn).Equals(2);

    REQUIRE_THROWS_HR(builder.Prepare(connection), MAKE_HRESULT(SEVERITY_ERROR, FACILITY_SQLITE, SQLITE_ERROR));
}

TEST_CASE("SQLiteWrapper_BusyTimeout_None", "[sqlitewrapper]")
{
    TestCommon::TempFile tempFile{ "repolibtest_tempdb"s, ".db"s };
    INFO("Using temporary file named: " << tempFile.GetPath());

    wil::unique_event busy, done;
    busy.create();
    done.create();

    std::thread busyThread([&]()
        {
            Connection threadConnection = Connection::Create(tempFile, Connection::OpenDisposition::Create);
            Statement threadStatement = Statement::Create(threadConnection, "BEGIN EXCLUSIVE TRANSACTION");
            threadStatement.Execute();
            busy.SetEvent();
            done.wait(500);
        });
    busyThread.detach();

    busy.wait(500);

    Connection testConnection = Connection::Create(tempFile, Connection::OpenDisposition::ReadWrite);
    testConnection.SetBusyTimeout(0ms);
    Statement testStatement = Statement::Create(testConnection, "BEGIN EXCLUSIVE TRANSACTION");
    REQUIRE_THROWS_HR(testStatement.Execute(), MAKE_HRESULT(SEVERITY_ERROR, FACILITY_SQLITE, SQLITE_BUSY));

    done.SetEvent();
}

TEST_CASE("SQLiteWrapper_BusyTimeout_Some", "[sqlitewrapper]")
{
    TestCommon::TempFile tempFile{ "repolibtest_tempdb"s, ".db"s };
    INFO("Using temporary file named: " << tempFile.GetPath());

    wil::unique_event busy, ready, done;
    busy.create();
    ready.create();
    done.create();

    std::thread busyThread([&]()
        {
            Connection threadConnection = Connection::Create(tempFile, Connection::OpenDisposition::Create);
            Statement threadBeginStatement = Statement::Create(threadConnection, "BEGIN EXCLUSIVE TRANSACTION");
            Statement threadCommitStatement = Statement::Create(threadConnection, "COMMIT");
            threadBeginStatement.Execute();
            busy.SetEvent();
            ready.wait(500);
            done.wait(100);
            threadCommitStatement.Execute();
        });
    busyThread.detach();

    busy.wait(500);

    Connection testConnection = Connection::Create(tempFile, Connection::OpenDisposition::ReadWrite);
    testConnection.SetBusyTimeout(500ms);
    Statement testStatement = Statement::Create(testConnection, "BEGIN EXCLUSIVE TRANSACTION");
    ready.SetEvent();
    testStatement.Execute();

    done.SetEvent();
}

TEST_CASE("SQLiteWrapper_CloseConnectionOnError", "[sqlitewrapper]")
{
    Connection connection = Connection::Create(SQLITE_MEMORY_DB_CONNECTION_TARGET, Connection::OpenDisposition::Create);

    Builder::StatementBuilder builder;
    builder.CreateTable(s_tableName).Columns({
        Builder::ColumnBuilder(s_firstColumn, Builder::Type::Int),
        Builder::ColumnBuilder(s_secondColumn, Builder::Type::Text),
        });

    Statement createTable = builder.Prepare(connection);
    REQUIRE_FALSE(createTable.Step());
    REQUIRE(createTable.GetState() == Statement::State::Completed);

    createTable.Reset();
    REQUIRE_THROWS(createTable.Step(true));

    // Do anything that needs the connection
    REQUIRE_THROWS_HR(connection.GetLastInsertRowID(), APPINSTALLER_CLI_ERROR_SQLITE_CONNECTION_TERMINATED);
}

TEST_CASE("SQLBuilder_SimpleSelectBind", "[sqlbuilder]")
{
    Connection connection = Connection::Create(SQLITE_MEMORY_DB_CONNECTION_TARGET, Connection::OpenDisposition::Create);

    CreateSimpleTestTable(connection);

    InsertIntoSimpleTestTable(connection, 1, "1");
    InsertIntoSimpleTestTable(connection, 2, "2");
    InsertIntoSimpleTestTable(connection, 3, "3");

    Builder::StatementBuilder builder;
    builder.Select({ s_firstColumn, s_secondColumn }).From(s_tableName).Where(s_firstColumn).Equals(2);

    auto statement = builder.Prepare(connection);

    REQUIRE(statement.Step());
    REQUIRE(statement.GetColumn<int>(0) == 2);
    REQUIRE(statement.GetColumn<std::string>(0) == "2");

    REQUIRE(!statement.Step());

    Builder::StatementBuilder buildCount;
    buildCount.Select(Builder::RowCount).From(s_tableName);

    auto rows = buildCount.Prepare(connection);

    REQUIRE(rows.Step());
    REQUIRE(rows.GetColumn<int>(0) == 3);

    REQUIRE(!rows.Step());
}

TEST_CASE("SQLBuilder_SimpleSelectUnbound", "[sqlbuilder]")
{
    Connection connection = Connection::Create(SQLITE_MEMORY_DB_CONNECTION_TARGET, Connection::OpenDisposition::Create);

    CreateSimpleTestTable(connection);

    InsertIntoSimpleTestTable(connection, 1, "1");
    InsertIntoSimpleTestTable(connection, 2, "2");
    InsertIntoSimpleTestTable(connection, 3, "3");

    Builder::StatementBuilder builder;
    builder.Select({ s_firstColumn, s_secondColumn }).From(s_tableName).Where(s_firstColumn).Equals(Builder::Unbound);

    auto statement = builder.Prepare(connection);

    statement.Bind(1, 2);

    REQUIRE(statement.Step());
    REQUIRE(statement.GetColumn<int>(0) == 2);
    REQUIRE(statement.GetColumn<std::string>(0) == "2");

    REQUIRE(!statement.Step());
}

TEST_CASE("SQLBuilder_SimpleSelectNull", "[sqlbuilder]")
{
    Connection connection = Connection::Create(SQLITE_MEMORY_DB_CONNECTION_TARGET, Connection::OpenDisposition::Create);

    CreateSimpleTestTable(connection);

    InsertIntoSimpleTestTable(connection, 1, "1");
    InsertIntoSimpleTestTable(connection, 2, "2");
    InsertIntoSimpleTestTableWithNull(connection, 3);

    Builder::StatementBuilder builder;
    builder.Select({ s_firstColumn, s_secondColumn }).From(s_tableName).Where(s_secondColumn).IsNull();

    auto statement = builder.Prepare(connection);

    REQUIRE(statement.Step());
    REQUIRE(statement.GetColumn<int>(0) == 3);
    REQUIRE(statement.GetColumnIsNull(1));

    REQUIRE(!statement.Step());
}

TEST_CASE("SQLBuilder_SimpleSelectOptional", "[sqlbuilder]")
{
    Connection connection = Connection::Create(SQLITE_MEMORY_DB_CONNECTION_TARGET, Connection::OpenDisposition::Create);

    CreateSimpleTestTable(connection);

    InsertIntoSimpleTestTable(connection, 1, "1");
    InsertIntoSimpleTestTable(connection, 2, "2");
    InsertIntoSimpleTestTableWithNull(connection, 3);

    std::optional<std::string> secondValue;

    {
        Builder::StatementBuilder builder;
        builder.Select({ s_firstColumn, s_secondColumn }).From(s_tableName).Where(s_secondColumn).Equals(secondValue);

        auto statement = builder.Prepare(connection);

        REQUIRE(statement.Step());
        REQUIRE(statement.GetColumn<int>(0) == 3);
        REQUIRE(statement.GetColumnIsNull(1));

        REQUIRE(!statement.Step());
    }

    {
        secondValue = "2";
        Builder::StatementBuilder builder;
        builder.Select({ s_firstColumn, s_secondColumn }).From(s_tableName).Where(s_secondColumn).Equals(secondValue);

        auto statement = builder.Prepare(connection);

        REQUIRE(statement.Step());
        REQUIRE(statement.GetColumn<int>(0) == 2);
        REQUIRE(statement.GetColumn<std::string>(1) == "2");

        REQUIRE(!statement.Step());
    }
}

TEST_CASE("SQLBuilder_Update", "[sqlbuilder]")
{
    Connection connection = Connection::Create(SQLITE_MEMORY_DB_CONNECTION_TARGET, Connection::OpenDisposition::Create);

    CreateSimpleTestTable(connection);

    int firstVal = 1;
    std::string secondVal = "test";

    InsertIntoSimpleTestTable(connection, firstVal, secondVal);

    SelectFromSimpleTestTableOnlyOneRow(connection, firstVal, secondVal);

    firstVal = 2;
    secondVal = "testing";

    UpdateSimpleTestTable(connection, firstVal, secondVal);

    SelectFromSimpleTestTableOnlyOneRow(connection, firstVal, secondVal);
}

TEST_CASE("SQLBuilder_CaseInsensitive", "[sqlbuilder]")
{
    Connection connection = Connection::Create(SQLITE_MEMORY_DB_CONNECTION_TARGET, Connection::OpenDisposition::Create);

    Builder::StatementBuilder createTable;
    createTable.CreateTable(s_tableName).Columns({
        Builder::ColumnBuilder(s_firstColumn, Builder::Type::Text).CollateNoCase()
        });

    createTable.Execute(connection);

    std::string upperCaseVal = "TEST";
    std::string lowerCaseVal = "test";

    {
        INFO("Insert initial value");
        Builder::StatementBuilder builder;
        builder.InsertInto(s_tableName)
            .Columns({ s_firstColumn })
            .Values(upperCaseVal);

        builder.Execute(connection);
    }

    {
        INFO("Retrieve using case-insensitive value");
        Builder::StatementBuilder builder;
        builder.Select({ s_firstColumn }).From(s_tableName).Where(s_firstColumn).Equals(lowerCaseVal);

        auto statement = builder.Prepare(connection);
        REQUIRE(statement.Step());
    }
}

TEST_CASE("SQLBuilder_CreateTable", "[sqlbuilder]")
{
    Connection connection = Connection::Create(SQLITE_MEMORY_DB_CONNECTION_TARGET, Connection::OpenDisposition::Create);

    int testRun = GENERATE(0, 1, 2, 3, 4, 5, 6, 7);

    bool notNull = ((testRun & 1) != 0);
    bool unique = ((testRun & 2) != 0);
    bool pk = ((testRun & 4) != 0);
    CAPTURE(notNull, unique, pk);

    Builder::StatementBuilder createTable;
    createTable.CreateTable(s_tableName).Columns({
        Builder::ColumnBuilder(s_firstColumn, Builder::Type::Int).NotNull(notNull).Unique(unique).PrimaryKey(pk)
        });

    createTable.Execute(connection);

    Builder::StatementBuilder insertBuilder;
    insertBuilder.InsertInto(s_tableName).Columns(s_firstColumn).Values(Builder::Unbound);

    Statement insertStatement = insertBuilder.Prepare(connection);

    {
        INFO("Insert NULL");
        insertStatement.Bind(1, nullptr);

        if (notNull)
        {
            REQUIRE_THROWS_HR(insertStatement.Execute(), MAKE_HRESULT(SEVERITY_ERROR, FACILITY_SQLITE, SQLITE_CONSTRAINT_NOTNULL));
        }
        else
        {
            insertStatement.Execute();
        }
    }

    {
        INFO("Insert unique values");
        insertStatement.Reset();
        insertStatement.Bind(1, 1);
        insertStatement.Execute();

        insertStatement.Reset();
        insertStatement.Bind(1, 2);
        insertStatement.Execute();
    }

    {
        INFO("Insert duplicate values");
        insertStatement.Reset();
        insertStatement.Bind(1, 1);

        if (unique || pk)
        {
            HRESULT expectedHR = S_OK;
            if (pk)
            {
                expectedHR = MAKE_HRESULT(SEVERITY_ERROR, FACILITY_SQLITE, SQLITE_CONSTRAINT_PRIMARYKEY);
            }
            else
            {
                expectedHR = MAKE_HRESULT(SEVERITY_ERROR, FACILITY_SQLITE, SQLITE_CONSTRAINT_UNIQUE);
            }
            REQUIRE_THROWS_HR(insertStatement.Execute(), expectedHR);
        }
        else
        {
            insertStatement.Execute();
        }
    }
}

TEST_CASE("SQLBuilder_InsertValueBinding", "[sqlbuilder]")
{
    char const* const columns[] = { "a", "b", "c", "d", "e", "f" };

    TestCommon::TempFile tempFile{ "repolibtest_tempdb"s, ".db"s };
    INFO("Using temporary file named: " << tempFile.GetPath());

    Connection connection = Connection::Create(tempFile, Connection::OpenDisposition::Create);

    {
        INFO("Create table");
        Builder::StatementBuilder createTable;
        createTable.CreateTable(s_tableName).BeginColumns();
        for (const auto c : columns)
        {
            createTable.Column(Builder::ColumnBuilder(c, Builder::Type::Int));
        }
        createTable.EndColumns();
        createTable.Execute(connection);
    }

    {
        INFO("Insert values");
        Builder::StatementBuilder insertBuilder;
        insertBuilder.InsertInto(s_tableName).BeginColumns();
        for (const auto c : columns)
        {
            insertBuilder.Column(c);
        }
        insertBuilder.EndColumns().Values(0, 1, 2, 3, 4, 5);
        insertBuilder.Execute(connection);
    }

    {
        INFO("Insert values");
        Builder::StatementBuilder insertBuilder;
        insertBuilder.InsertInto(s_tableName).BeginColumns();
        for (const auto c : columns)
        {
            insertBuilder.Column(c);
        }
        insertBuilder.EndColumns().BeginValues();
        insertBuilder.Value(5);
        insertBuilder.Value(nullptr);
        insertBuilder.Value(3);
        insertBuilder.Value(std::optional<int>{});
        insertBuilder.Value(std::optional<int>{ 1 });
        insertBuilder.Value(Builder::Unbound);
        insertBuilder.EndValues();
        insertBuilder.Execute(connection);
    }

    {
        INFO("Select values");
        Builder::StatementBuilder selectBuilder;
        selectBuilder.Select();
        for (const auto c : columns)
        {
            selectBuilder.Column(c);
        }
        selectBuilder.From(s_tableName);

        Statement select = selectBuilder.Prepare(connection);
        REQUIRE(select.Step());

        for (int i = 0; i < ARRAYSIZE(columns); ++i)
        {
            REQUIRE(i == select.GetColumn<int>(i));
        }

        REQUIRE(select.Step());

        for (int i = 0; i < ARRAYSIZE(columns); ++i)
        {
            if (i & 1)
            {
                REQUIRE(select.GetColumnIsNull(i));
            }
            else
            {
                REQUIRE((5 - i) == select.GetColumn<int>(i));
            }
        }

        REQUIRE(!select.Step());
    }
}

TEST_CASE("SQLiteWrapperTransactionRollback", "[sqlitewrapper]")
{
    Connection connection = Connection::Create(SQLITE_MEMORY_DB_CONNECTION_TARGET, Connection::OpenDisposition::Create);

    int firstVal = 1;
    std::string secondVal = "test";

    CreateSimpleTestTable(connection);

    Transaction transaction = Transaction::Create(connection, "test_transaction", false);

    InsertIntoSimpleTestTable(connection, firstVal, secondVal);

    transaction.Rollback();

    Statement select = Statement::Create(connection, s_selectFromSimpleTestTableSQL);
    REQUIRE(!select.Step());
    REQUIRE(select.GetState() == Statement::State::Completed);
}

TEST_CASE("SQLiteWrapperTransactionRollbackOnDestruct", "[sqlitewrapper]")
{
    Connection connection = Connection::Create(SQLITE_MEMORY_DB_CONNECTION_TARGET, Connection::OpenDisposition::Create);

    int firstVal = 1;
    std::string secondVal = "test";

    CreateSimpleTestTable(connection);

    {
        Transaction transaction = Transaction::Create(connection, "test_transaction", false);

        InsertIntoSimpleTestTable(connection, firstVal, secondVal);
    }

    Statement select = Statement::Create(connection, s_selectFromSimpleTestTableSQL);
    REQUIRE(!select.Step());
    REQUIRE(select.GetState() == Statement::State::Completed);
}

TEST_CASE("SQLiteWrapperTransactionCommit", "[sqlitewrapper]")
{
    Connection connection = Connection::Create(SQLITE_MEMORY_DB_CONNECTION_TARGET, Connection::OpenDisposition::Create);

    int firstVal = 1;
    std::string secondVal = "test";

    CreateSimpleTestTable(connection);

    {
        Transaction transaction = Transaction::Create(connection, "test_transaction", false);

        InsertIntoSimpleTestTable(connection, firstVal, secondVal);

        transaction.Commit();
    }

    SelectFromSimpleTestTableOnlyOneRow(connection, firstVal, secondVal);
}

TEST_CASE("SQLiteWrapperTransactionImmediate", "[sqlitewrapper]")
{
    Connection connection = Connection::Create(SQLITE_MEMORY_DB_CONNECTION_TARGET, Connection::OpenDisposition::Create);

    int firstVal = 1;
    std::string secondVal = "test";

    CreateSimpleTestTable(connection);

    {
        Transaction transaction = Transaction::Create(connection, "test_transaction", true);

        InsertIntoSimpleTestTable(connection, firstVal, secondVal);

        transaction.Commit();
    }

    SelectFromSimpleTestTableOnlyOneRow(connection, firstVal, secondVal);
}

TEST_CASE("SQLiteWrapperTransactionWriteConflict", "[sqlitewrapper]")
{
    TestCommon::TempFile tempFile{ "repolibtest_tempdb"s, ".db"s };
    INFO("Using temporary file named: " << tempFile.GetPath());

    Connection connection = Connection::Create(tempFile, Connection::OpenDisposition::Create);
    connection.SetJournalMode("WAL");

    int firstVal = 1;
    std::string secondVal = "test";

    CreateSimpleTestTable(connection);

    Connection connection2 = Connection::Create(tempFile, Connection::OpenDisposition::ReadWrite);
    std::chrono::milliseconds busyWait = 250ms;
    connection2.SetBusyTimeout(busyWait);

    {
        Transaction transaction = Transaction::Create(connection, "test_transaction", true);
        InsertIntoSimpleTestTable(connection, firstVal, secondVal);

        // Start second transaction
        std::chrono::system_clock::time_point start = std::chrono::system_clock::now();
        std::chrono::system_clock::time_point end = start;
        try
        {
            Transaction transaction2 = Transaction::Create(connection2, "test_transaction2", true);
        }
        catch (...)
        {
            end = std::chrono::system_clock::now();
        }

        std::chrono::milliseconds duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        REQUIRE(duration >= busyWait);

        transaction.Commit();

        Transaction transaction2 = Transaction::Create(connection2, "test_transaction2", true);
        InsertIntoSimpleTestTable(connection2, firstVal, secondVal);
    }

    SelectFromSimpleTestTableOnlyOneRow(connection, firstVal, secondVal);
}
