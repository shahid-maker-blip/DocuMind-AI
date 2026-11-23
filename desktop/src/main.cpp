
#include "imgui.h"
#include "backends/imgui_impl_sdl2.h"
#include "backends/imgui_impl_sdlrenderer2.h"

#include <SDL.h>
#include <sqlite3.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>
#include <filesystem>
#include <ctime>
#include <sstream>
#include <iomanip>
#include <cctype>
#include <fstream>
#include <iterator>
#include <cstdint>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#include <commdlg.h>
#include <winhttp.h>
#endif

namespace fs = std::filesystem;

static std::string statusMessage = "Welcome to DocuMind AI";
static int selectedPage = 0;
static bool gDarkTheme = false;

static const float kSidebarWidth = 235.0f;
static const float kAssistantWidth = 340.0f;

static sqlite3* gDatabase = nullptr;
static fs::path gProjectRoot;
static int pendingDeleteId = -1;

struct Document
{
    int id = 0;
    std::string name;
    std::string path;
    std::string type;
    long long size = 0;
    std::string addedAt;
    int categoryId = 0;
    std::string categoryName = "Uncategorized";
    std::vector<std::string> tags;
};

struct DocumentCategory
{
    int id = 0;
    std::string name;
};

struct DuplicateDocumentGroup
{
    std::vector<Document> documents;
};

static std::vector<DuplicateDocumentGroup> gDuplicateGroups;
static bool gDuplicateScanCompleted = false;
static int gDuplicateFilesChecked = 0;
static int gDuplicateMissingFiles = 0;

static std::vector<Document> gDocuments;
static void LoadDocuments();
static std::vector<DocumentCategory> gCategories;
static char documentSearch[256] = "";
static int gCategoryFilterId = 0;
static int gOrganizationDocumentId = -1;
static int gEditDocumentCategoryId = 0;
static char gDocumentTagInput[128] = "";
static char gNewCategoryInput[128] = "";
static char gRenameCategoryInput[128] = "";
static int gSelectedCategoryToEdit = -1;

static bool gOpenReaderPopup = false;
static std::string gExtractedDocumentText;
static std::string gExtractedDocumentName;

struct DocumentSummary
{
    int id = 0;
    std::string filename;
    std::string summaryType;
    std::string text;
    std::string generatedAt;
};

struct ChatEntry
{
    std::string role;
    std::string content;
};

struct ChatConversation
{
    int id = 0;
    int documentId = -1;
    std::string title;
    std::string documentName;
    std::string createdAt;
    std::string updatedAt;
};

static std::vector<ChatEntry> gChatMessages;
static std::vector<ChatConversation> gChatConversations;
static int gCurrentChatConversationId = -1;
static int gSelectedSavedConversationId = -1;
static int gSelectedChatDocumentId = -1;
static char gChatQuestion[1024] = "";

static int gSelectedKeyInfoDocumentId = -1;
static std::string gKeyInformationReport;

static int gSelectedCompareDocumentA = -1;
static int gSelectedCompareDocumentB = -1;
static std::string gDocumentComparisonReport;

static std::vector<DocumentSummary> gSummaries;
static int gSelectedSummaryId = -1;
static char summarySearch[256] = "";

static std::string FormatFileSize(long long bytes)
{
    std::ostringstream output;

    if (bytes < 1024)
        output << bytes << " B";
    else if (bytes < 1024LL * 1024)
        output << std::fixed << std::setprecision(2)
               << bytes / 1024.0 << " KB";
    else
        output << std::fixed << std::setprecision(2)
               << bytes / (1024.0 * 1024.0) << " MB";

    return output.str();
}

static std::string GetCurrentDateTime()
{
    std::time_t now = std::time(nullptr);
    std::tm localTime{};

#ifdef _WIN32
    localtime_s(&localTime, &now);
#else
    localtime_r(&now, &localTime);
#endif

    char buffer[64];

    std::strftime(
        buffer,
        sizeof(buffer),
        "%Y-%m-%d %H:%M",
        &localTime
    );

    return buffer;
}

static bool InitializeDatabase()
{
    const char* basePath = SDL_GetBasePath();

    if (!basePath)
    {
        statusMessage = "Could not locate application directory.";
        return false;
    }

    gProjectRoot = fs::path(basePath).parent_path();

    fs::create_directories(gProjectRoot / "documents");
    fs::create_directories(gProjectRoot / "database");

    fs::path databasePath =
        gProjectRoot / "database" / "documind.db";

    if (sqlite3_open(databasePath.string().c_str(), &gDatabase)
        != SQLITE_OK)
    {
        statusMessage = gDatabase
            ? sqlite3_errmsg(gDatabase)
            : "Could not open database.";

        if (gDatabase)
        {
            sqlite3_close(gDatabase);
            gDatabase = nullptr;
        }

        return false;
    }

    const char* sql = R"(
        CREATE TABLE IF NOT EXISTS documents
        (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            name TEXT NOT NULL,
            path TEXT NOT NULL,
            type TEXT NOT NULL,
            size INTEGER NOT NULL,
            added_at TEXT NOT NULL
        );
    )";

    char* errorMessage = nullptr;

    if (sqlite3_exec(
        gDatabase,
        sql,
        nullptr,
        nullptr,
        &errorMessage
    ) != SQLITE_OK)
    {
        statusMessage = errorMessage
            ? errorMessage
            : "Database table creation failed.";

        sqlite3_free(errorMessage);
        return false;
    }

    const char* summariesSql = R"(
        CREATE TABLE IF NOT EXISTS summaries
        (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            filename TEXT NOT NULL,
            summary_type TEXT NOT NULL,
            summary_text TEXT NOT NULL,
            generated_at TEXT NOT NULL
        );
    )";

    errorMessage = nullptr;

    if (sqlite3_exec(
        gDatabase,
        summariesSql,
        nullptr,
        nullptr,
        &errorMessage
    ) != SQLITE_OK)
    {
        statusMessage = errorMessage
            ? errorMessage
            : "Summary history table creation failed.";

        sqlite3_free(errorMessage);
        return false;
    }

    // These additional tables store AI chat conversations without changing
    // existing document or summary records.
    const char* chatTablesSql = R"(
        CREATE TABLE IF NOT EXISTS chat_conversations
        (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            document_id INTEGER NOT NULL,
            document_name TEXT NOT NULL,
            title TEXT NOT NULL,
            created_at TEXT NOT NULL,
            updated_at TEXT NOT NULL
        );

        CREATE TABLE IF NOT EXISTS chat_messages
        (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            conversation_id INTEGER NOT NULL,
            role TEXT NOT NULL,
            content TEXT NOT NULL,
            created_at TEXT NOT NULL
        );

        CREATE INDEX IF NOT EXISTS idx_chat_messages_conversation
        ON chat_messages(conversation_id, id);
    )";

    errorMessage = nullptr;
    if (sqlite3_exec(
        gDatabase,
        chatTablesSql,
        nullptr,
        nullptr,
        &errorMessage
    ) != SQLITE_OK)
    {
        statusMessage = errorMessage
            ? errorMessage
            : "Chat history tables could not be created.";
        sqlite3_free(errorMessage);
        return false;
    }

    // Document organization tables are added without replacing existing data.
    const char* organizationSql = R"(
        CREATE TABLE IF NOT EXISTS categories
        (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            name TEXT NOT NULL COLLATE NOCASE UNIQUE,
            created_at TEXT NOT NULL
        );

        CREATE TABLE IF NOT EXISTS document_tags
        (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            document_id INTEGER NOT NULL,
            tag TEXT NOT NULL COLLATE NOCASE,
            created_at TEXT NOT NULL,
            UNIQUE(document_id, tag)
        );

        CREATE INDEX IF NOT EXISTS idx_document_tags_document
        ON document_tags(document_id);

        INSERT OR IGNORE INTO categories(name, created_at) VALUES
            ('Uncategorized', 'System'),
            ('Business', 'System'),
            ('Education', 'System'),
            ('Finance', 'System'),
            ('Legal', 'System'),
            ('Personal', 'System');
    )";

    errorMessage = nullptr;
    if (sqlite3_exec(gDatabase, organizationSql, nullptr, nullptr, &errorMessage) != SQLITE_OK)
    {
        statusMessage = errorMessage ? errorMessage : "Organization tables could not be created.";
        sqlite3_free(errorMessage);
        return false;
    }

    // Upgrade older databases safely: add category_id only when it is absent.
    bool hasCategoryId = false;
    sqlite3_stmt* columnsStatement = nullptr;
    if (sqlite3_prepare_v2(gDatabase, "PRAGMA table_info(documents);", -1, &columnsStatement, nullptr) != SQLITE_OK)
    {
        statusMessage = sqlite3_errmsg(gDatabase);
        return false;
    }
    while (sqlite3_step(columnsStatement) == SQLITE_ROW)
    {
        const unsigned char* columnName = sqlite3_column_text(columnsStatement, 1);
        if (columnName && std::string(reinterpret_cast<const char*>(columnName)) == "category_id")
            hasCategoryId = true;
    }
    sqlite3_finalize(columnsStatement);

    if (!hasCategoryId && sqlite3_exec(gDatabase,
        "ALTER TABLE documents ADD COLUMN category_id INTEGER;", nullptr, nullptr, &errorMessage) != SQLITE_OK)
    {
        statusMessage = errorMessage ? errorMessage : "Could not add document categories.";
        sqlite3_free(errorMessage);
        return false;
    }

    if (sqlite3_exec(gDatabase,
        "UPDATE documents SET category_id = (SELECT id FROM categories WHERE name = 'Uncategorized') WHERE category_id IS NULL;"
        "CREATE INDEX IF NOT EXISTS idx_documents_category ON documents(category_id);",
        nullptr, nullptr, &errorMessage) != SQLITE_OK)
    {
        statusMessage = errorMessage ? errorMessage : "Could not initialize document categories.";
        sqlite3_free(errorMessage);
        return false;
    }

    return true;
}

static void LoadCategories()
{
    gCategories.clear();
    if (!gDatabase) return;

    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(gDatabase, "SELECT id, name FROM categories ORDER BY CASE WHEN name = 'Uncategorized' THEN 0 ELSE 1 END, name COLLATE NOCASE;", -1, &statement, nullptr) != SQLITE_OK)
    {
        statusMessage = sqlite3_errmsg(gDatabase);
        return;
    }
    while (sqlite3_step(statement) == SQLITE_ROW)
    {
        DocumentCategory category;
        category.id = sqlite3_column_int(statement, 0);
        const unsigned char* name = sqlite3_column_text(statement, 1);
        category.name = name ? reinterpret_cast<const char*>(name) : "";
        gCategories.push_back(std::move(category));
    }
    sqlite3_finalize(statement);
}

static int GetUncategorizedCategoryId()
{
    for (const DocumentCategory& category : gCategories)
        if (category.name == "Uncategorized") return category.id;
    return gCategories.empty() ? 0 : gCategories.front().id;
}

static std::string TrimText(const std::string& value)
{
    const size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    const size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

static bool UpdateDocumentCategory(int documentId, int categoryId)
{
    sqlite3_stmt* statement = nullptr;
    if (!gDatabase || sqlite3_prepare_v2(gDatabase, "UPDATE documents SET category_id = ? WHERE id = ?;", -1, &statement, nullptr) != SQLITE_OK)
    {
        statusMessage = gDatabase ? sqlite3_errmsg(gDatabase) : "Database is not available.";
        return false;
    }
    sqlite3_bind_int(statement, 1, categoryId);
    sqlite3_bind_int(statement, 2, documentId);
    const bool success = sqlite3_step(statement) == SQLITE_DONE;
    sqlite3_finalize(statement);
    if (!success) statusMessage = sqlite3_errmsg(gDatabase);
    else { LoadDocuments(); statusMessage = "Document category updated."; }
    return success;
}

static bool AddDocumentTag(int documentId, const std::string& rawTag)
{
    const std::string tag = TrimText(rawTag);
    if (tag.empty() || tag.size() > 64) { statusMessage = "Enter a tag between 1 and 64 characters."; return false; }
    sqlite3_stmt* statement = nullptr;
    if (!gDatabase || sqlite3_prepare_v2(gDatabase, "INSERT OR IGNORE INTO document_tags(document_id, tag, created_at) VALUES(?, ?, ?);", -1, &statement, nullptr) != SQLITE_OK)
    { statusMessage = gDatabase ? sqlite3_errmsg(gDatabase) : "Database is not available."; return false; }
    sqlite3_bind_int(statement, 1, documentId);
    sqlite3_bind_text(statement, 2, tag.c_str(), -1, SQLITE_TRANSIENT);
    const std::string now = GetCurrentDateTime();
    sqlite3_bind_text(statement, 3, now.c_str(), -1, SQLITE_TRANSIENT);
    const bool success = sqlite3_step(statement) == SQLITE_DONE;
    sqlite3_finalize(statement);
    if (!success) statusMessage = sqlite3_errmsg(gDatabase);
    else { LoadDocuments(); statusMessage = "Tag added to document."; }
    return success;
}

static bool RemoveDocumentTag(int documentId, const std::string& tag)
{
    sqlite3_stmt* statement = nullptr;
    if (!gDatabase || sqlite3_prepare_v2(gDatabase, "DELETE FROM document_tags WHERE document_id = ? AND tag = ?;", -1, &statement, nullptr) != SQLITE_OK)
    { statusMessage = gDatabase ? sqlite3_errmsg(gDatabase) : "Database is not available."; return false; }
    sqlite3_bind_int(statement, 1, documentId);
    sqlite3_bind_text(statement, 2, tag.c_str(), -1, SQLITE_TRANSIENT);
    const bool success = sqlite3_step(statement) == SQLITE_DONE;
    sqlite3_finalize(statement);
    if (!success) statusMessage = sqlite3_errmsg(gDatabase);
    else { LoadDocuments(); statusMessage = "Tag removed."; }
    return success;
}

static bool AddCategory(const std::string& rawName)
{
    const std::string name = TrimText(rawName);
    if (name.empty() || name.size() > 40) { statusMessage = "Category names must be 1 to 40 characters."; return false; }
    sqlite3_stmt* statement = nullptr;
    if (!gDatabase || sqlite3_prepare_v2(gDatabase, "INSERT INTO categories(name, created_at) VALUES(?, ?);", -1, &statement, nullptr) != SQLITE_OK)
    { statusMessage = gDatabase ? sqlite3_errmsg(gDatabase) : "Database is not available."; return false; }
    sqlite3_bind_text(statement, 1, name.c_str(), -1, SQLITE_TRANSIENT);
    const std::string now = GetCurrentDateTime();
    sqlite3_bind_text(statement, 2, now.c_str(), -1, SQLITE_TRANSIENT);
    const bool success = sqlite3_step(statement) == SQLITE_DONE;
    sqlite3_finalize(statement);
    if (!success) statusMessage = "That category already exists, or could not be created.";
    else { LoadCategories(); statusMessage = "Category created."; }
    return success;
}

static bool RenameCategory(int categoryId, const std::string& rawName)
{
    if (categoryId == GetUncategorizedCategoryId())
    {
        statusMessage = "The Uncategorized category name is reserved.";
        return false;
    }
    const std::string name = TrimText(rawName);
    if (name.empty() || name.size() > 40) { statusMessage = "Category names must be 1 to 40 characters."; return false; }
    sqlite3_stmt* statement = nullptr;
    if (!gDatabase || sqlite3_prepare_v2(gDatabase, "UPDATE categories SET name = ? WHERE id = ?;", -1, &statement, nullptr) != SQLITE_OK)
    { statusMessage = gDatabase ? sqlite3_errmsg(gDatabase) : "Database is not available."; return false; }
    sqlite3_bind_text(statement, 1, name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(statement, 2, categoryId);
    const bool success = sqlite3_step(statement) == SQLITE_DONE;
    sqlite3_finalize(statement);
    if (!success) statusMessage = "That category name already exists, or could not be renamed.";
    else { LoadCategories(); LoadDocuments(); statusMessage = "Category renamed."; }
    return success;
}

static bool DeleteCategory(int categoryId)
{
    const int uncategorizedId = GetUncategorizedCategoryId();
    if (categoryId == uncategorizedId) { statusMessage = "Uncategorized cannot be removed."; return false; }
    if (!gDatabase) return false;
    if (sqlite3_exec(gDatabase, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) { statusMessage = sqlite3_errmsg(gDatabase); return false; }
    sqlite3_stmt* update = nullptr;
    sqlite3_stmt* remove = nullptr;
    bool success = sqlite3_prepare_v2(gDatabase, "UPDATE documents SET category_id = ? WHERE category_id = ?;", -1, &update, nullptr) == SQLITE_OK;
    if (success) { sqlite3_bind_int(update, 1, uncategorizedId); sqlite3_bind_int(update, 2, categoryId); success = sqlite3_step(update) == SQLITE_DONE; }
    if (update) sqlite3_finalize(update);
    if (success) success = sqlite3_prepare_v2(gDatabase, "DELETE FROM categories WHERE id = ?;", -1, &remove, nullptr) == SQLITE_OK;
    if (success) { sqlite3_bind_int(remove, 1, categoryId); success = sqlite3_step(remove) == SQLITE_DONE; }
    if (remove) sqlite3_finalize(remove);
    if (success) success = sqlite3_exec(gDatabase, "COMMIT;", nullptr, nullptr, nullptr) == SQLITE_OK;
    else sqlite3_exec(gDatabase, "ROLLBACK;", nullptr, nullptr, nullptr);
    if (!success) { sqlite3_exec(gDatabase, "ROLLBACK;", nullptr, nullptr, nullptr); statusMessage = sqlite3_errmsg(gDatabase); return false; }
    LoadCategories(); LoadDocuments();
    if (gCategoryFilterId == categoryId) gCategoryFilterId = 0;
    statusMessage = "Category removed. Its documents were moved to Uncategorized.";
    return true;
}

static void LoadDocuments()
{
    gDocuments.clear();

    if (!gDatabase)
        return;

    const char* sql =
        "SELECT d.id, d.name, d.path, d.type, d.size, d.added_at, "
        "COALESCE(d.category_id, 0), COALESCE(c.name, 'Uncategorized') "
        "FROM documents d LEFT JOIN categories c ON c.id = d.category_id "
        "ORDER BY d.id DESC;";

    sqlite3_stmt* statement = nullptr;

    if (sqlite3_prepare_v2(
        gDatabase,
        sql,
        -1,
        &statement,
        nullptr
    ) != SQLITE_OK)
    {
        statusMessage = sqlite3_errmsg(gDatabase);
        return;
    }

    while (sqlite3_step(statement) == SQLITE_ROW)
    {
        Document document;

        document.id = sqlite3_column_int(statement, 0);

        const unsigned char* name =
            sqlite3_column_text(statement, 1);

        const unsigned char* path =
            sqlite3_column_text(statement, 2);

        const unsigned char* type =
            sqlite3_column_text(statement, 3);

        document.size =
            sqlite3_column_int64(statement, 4);

        const unsigned char* date =
            sqlite3_column_text(statement, 5);
        document.categoryId = sqlite3_column_int(statement, 6);
        const unsigned char* categoryName = sqlite3_column_text(statement, 7);

        document.name = name
            ? reinterpret_cast<const char*>(name) : "";

        document.path = path
            ? reinterpret_cast<const char*>(path) : "";

        document.type = type
            ? reinterpret_cast<const char*>(type) : "";

        document.addedAt = date
            ? reinterpret_cast<const char*>(date) : "";
        document.categoryName = categoryName
            ? reinterpret_cast<const char*>(categoryName) : "Uncategorized";

        gDocuments.push_back(std::move(document));
    }

    sqlite3_finalize(statement);

    sqlite3_stmt* tagStatement = nullptr;
    if (sqlite3_prepare_v2(gDatabase, "SELECT document_id, tag FROM document_tags ORDER BY tag COLLATE NOCASE;", -1, &tagStatement, nullptr) == SQLITE_OK)
    {
        while (sqlite3_step(tagStatement) == SQLITE_ROW)
        {
            const int documentId = sqlite3_column_int(tagStatement, 0);
            const unsigned char* tagText = sqlite3_column_text(tagStatement, 1);
            if (!tagText) continue;
            const std::string tag = reinterpret_cast<const char*>(tagText);
            for (Document& document : gDocuments)
            {
                if (document.id == documentId) { document.tags.push_back(tag); break; }
            }
        }
        sqlite3_finalize(tagStatement);
    }
}

static void LoadSummaries()
{
    gSummaries.clear();

    if (!gDatabase)
        return;

    const char* sql =
        "SELECT id, filename, summary_type, summary_text, generated_at "
        "FROM summaries ORDER BY id DESC;";

    sqlite3_stmt* statement = nullptr;

    if (sqlite3_prepare_v2(
        gDatabase,
        sql,
        -1,
        &statement,
        nullptr
    ) != SQLITE_OK)
    {
        statusMessage = sqlite3_errmsg(gDatabase);
        return;
    }

    while (sqlite3_step(statement) == SQLITE_ROW)
    {
        DocumentSummary item;

        item.id = sqlite3_column_int(statement, 0);

        const unsigned char* filename =
            sqlite3_column_text(statement, 1);
        const unsigned char* summaryType =
            sqlite3_column_text(statement, 2);
        const unsigned char* summaryText =
            sqlite3_column_text(statement, 3);
        const unsigned char* generatedAt =
            sqlite3_column_text(statement, 4);

        item.filename = filename
            ? reinterpret_cast<const char*>(filename) : "";
        item.summaryType = summaryType
            ? reinterpret_cast<const char*>(summaryType) : "";
        item.text = summaryText
            ? reinterpret_cast<const char*>(summaryText) : "";
        item.generatedAt = generatedAt
            ? reinterpret_cast<const char*>(generatedAt) : "";

        gSummaries.push_back(std::move(item));
    }

    sqlite3_finalize(statement);

    if (gSelectedSummaryId != -1)
    {
        bool found = false;

        for (const DocumentSummary& item : gSummaries)
        {
            if (item.id == gSelectedSummaryId)
            {
                found = true;
                break;
            }
        }

        if (!found)
            gSelectedSummaryId = -1;
    }
}

static void LoadChatConversations()
{
    gChatConversations.clear();

    if (!gDatabase)
        return;

    const char* sql =
        "SELECT id, document_id, document_name, title, created_at, updated_at "
        "FROM chat_conversations ORDER BY updated_at DESC, id DESC;";

    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(gDatabase, sql, -1, &statement, nullptr) != SQLITE_OK)
    {
        statusMessage = sqlite3_errmsg(gDatabase);
        return;
    }

    while (sqlite3_step(statement) == SQLITE_ROW)
    {
        ChatConversation item;
        item.id = sqlite3_column_int(statement, 0);
        item.documentId = sqlite3_column_int(statement, 1);

        const unsigned char* documentName = sqlite3_column_text(statement, 2);
        const unsigned char* title = sqlite3_column_text(statement, 3);
        const unsigned char* createdAt = sqlite3_column_text(statement, 4);
        const unsigned char* updatedAt = sqlite3_column_text(statement, 5);

        item.documentName = documentName ? reinterpret_cast<const char*>(documentName) : "";
        item.title = title ? reinterpret_cast<const char*>(title) : "Untitled conversation";
        item.createdAt = createdAt ? reinterpret_cast<const char*>(createdAt) : "";
        item.updatedAt = updatedAt ? reinterpret_cast<const char*>(updatedAt) : "";
        gChatConversations.push_back(std::move(item));
    }

    sqlite3_finalize(statement);

    if (gSelectedSavedConversationId != -1)
    {
        bool found = false;
        for (const ChatConversation& item : gChatConversations)
        {
            if (item.id == gSelectedSavedConversationId)
            {
                found = true;
                break;
            }
        }
        if (!found)
            gSelectedSavedConversationId = -1;
    }
}

static bool SaveChatTurn(
    int documentId,
    const std::string& documentName,
    const std::string& question,
    const std::string& answer)
{
    if (!gDatabase)
    {
        statusMessage = "Database is not available. Chat was not saved.";
        return false;
    }

    if (gCurrentChatConversationId == -1)
    {
        std::string title = question;
        if (title.size() > 80)
            title = title.substr(0, 77) + "...";
        if (title.empty())
            title = "New conversation";

        const char* insertConversationSql =
            "INSERT INTO chat_conversations "
            "(document_id, document_name, title, created_at, updated_at) "
            "VALUES (?, ?, ?, ?, ?);";

        sqlite3_stmt* createStatement = nullptr;
        if (sqlite3_prepare_v2(
            gDatabase, insertConversationSql, -1, &createStatement, nullptr
        ) != SQLITE_OK)
        {
            statusMessage = sqlite3_errmsg(gDatabase);
            return false;
        }

        const std::string now = GetCurrentDateTime();
        sqlite3_bind_int(createStatement, 1, documentId);
        sqlite3_bind_text(createStatement, 2, documentName.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(createStatement, 3, title.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(createStatement, 4, now.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(createStatement, 5, now.c_str(), -1, SQLITE_TRANSIENT);

        bool created = sqlite3_step(createStatement) == SQLITE_DONE;
        sqlite3_finalize(createStatement);
        if (!created)
        {
            statusMessage = sqlite3_errmsg(gDatabase);
            return false;
        }

        gCurrentChatConversationId =
            static_cast<int>(sqlite3_last_insert_rowid(gDatabase));
        gSelectedSavedConversationId = gCurrentChatConversationId;
    }

    const char* insertMessageSql =
        "INSERT INTO chat_messages "
        "(conversation_id, role, content, created_at) VALUES (?, ?, ?, ?);";

    sqlite3_stmt* messageStatement = nullptr;
    if (sqlite3_prepare_v2(
        gDatabase, insertMessageSql, -1, &messageStatement, nullptr
    ) != SQLITE_OK)
    {
        statusMessage = sqlite3_errmsg(gDatabase);
        return false;
    }

    const std::string now = GetCurrentDateTime();
    const ChatEntry turnEntries[2] = {
        {"user", question},
        {"assistant", answer}
    };

    bool saved = true;
    for (const ChatEntry& entry : turnEntries)
    {
        sqlite3_reset(messageStatement);
        sqlite3_clear_bindings(messageStatement);
        sqlite3_bind_int(messageStatement, 1, gCurrentChatConversationId);
        sqlite3_bind_text(messageStatement, 2, entry.role.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(messageStatement, 3, entry.content.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(messageStatement, 4, now.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(messageStatement) != SQLITE_DONE)
        {
            saved = false;
            break;
        }
    }
    sqlite3_finalize(messageStatement);

    if (!saved)
    {
        statusMessage = sqlite3_errmsg(gDatabase);
        return false;
    }

    sqlite3_stmt* updateStatement = nullptr;
    if (sqlite3_prepare_v2(
        gDatabase,
        "UPDATE chat_conversations SET updated_at = ? WHERE id = ?;",
        -1,
        &updateStatement,
        nullptr
    ) == SQLITE_OK)
    {
        sqlite3_bind_text(updateStatement, 1, now.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(updateStatement, 2, gCurrentChatConversationId);
        sqlite3_step(updateStatement);
        sqlite3_finalize(updateStatement);
    }

    LoadChatConversations();
    gSelectedSavedConversationId = gCurrentChatConversationId;
    return true;
}

static bool ExtractDocumentText(const Document& document);

static bool LoadChatConversation(int conversationId)
{
    if (!gDatabase)
        return false;

    int documentId = -1;
    sqlite3_stmt* conversationStatement = nullptr;
    if (sqlite3_prepare_v2(
        gDatabase,
        "SELECT document_id FROM chat_conversations WHERE id = ?;",
        -1,
        &conversationStatement,
        nullptr
    ) != SQLITE_OK)
    {
        statusMessage = sqlite3_errmsg(gDatabase);
        return false;
    }

    sqlite3_bind_int(conversationStatement, 1, conversationId);
    if (sqlite3_step(conversationStatement) == SQLITE_ROW)
        documentId = sqlite3_column_int(conversationStatement, 0);
    sqlite3_finalize(conversationStatement);

    if (documentId == -1)
    {
        statusMessage = "The selected saved conversation could not be found.";
        return false;
    }

    const Document* document = nullptr;
    for (const Document& item : gDocuments)
    {
        if (item.id == documentId)
        {
            document = &item;
            break;
        }
    }

    if (!document)
    {
        statusMessage = "The document for this conversation is no longer in your library.";
        return false;
    }

    if (!ExtractDocumentText(*document))
        return false;

    gOpenReaderPopup = false;

    std::vector<ChatEntry> loadedMessages;
    sqlite3_stmt* messagesStatement = nullptr;
    if (sqlite3_prepare_v2(
        gDatabase,
        "SELECT role, content FROM chat_messages "
        "WHERE conversation_id = ? ORDER BY id ASC;",
        -1,
        &messagesStatement,
        nullptr
    ) != SQLITE_OK)
    {
        statusMessage = sqlite3_errmsg(gDatabase);
        return false;
    }

    sqlite3_bind_int(messagesStatement, 1, conversationId);
    while (sqlite3_step(messagesStatement) == SQLITE_ROW)
    {
        const unsigned char* role = sqlite3_column_text(messagesStatement, 0);
        const unsigned char* content = sqlite3_column_text(messagesStatement, 1);
        loadedMessages.push_back({
            role ? reinterpret_cast<const char*>(role) : "assistant",
            content ? reinterpret_cast<const char*>(content) : ""
        });
    }
    sqlite3_finalize(messagesStatement);

    gSelectedChatDocumentId = documentId;
    gCurrentChatConversationId = conversationId;
    gSelectedSavedConversationId = conversationId;
    gChatMessages = std::move(loadedMessages);
    gChatQuestion[0] = '\0';
    statusMessage = "Saved conversation loaded. You can continue asking questions.";
    return true;
}

static bool DeleteChatConversation(int conversationId)
{
    if (!gDatabase)
        return false;

    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(
        gDatabase,
        "DELETE FROM chat_messages WHERE conversation_id = ?;",
        -1,
        &statement,
        nullptr
    ) != SQLITE_OK)
    {
        statusMessage = sqlite3_errmsg(gDatabase);
        return false;
    }
    sqlite3_bind_int(statement, 1, conversationId);
    bool messagesDeleted = sqlite3_step(statement) == SQLITE_DONE;
    sqlite3_finalize(statement);
    if (!messagesDeleted)
    {
        statusMessage = sqlite3_errmsg(gDatabase);
        return false;
    }

    if (sqlite3_prepare_v2(
        gDatabase,
        "DELETE FROM chat_conversations WHERE id = ?;",
        -1,
        &statement,
        nullptr
    ) != SQLITE_OK)
    {
        statusMessage = sqlite3_errmsg(gDatabase);
        return false;
    }
    sqlite3_bind_int(statement, 1, conversationId);
    bool conversationDeleted = sqlite3_step(statement) == SQLITE_DONE;
    sqlite3_finalize(statement);

    if (!conversationDeleted)
    {
        statusMessage = sqlite3_errmsg(gDatabase);
        return false;
    }

    if (gCurrentChatConversationId == conversationId)
    {
        gCurrentChatConversationId = -1;
        gChatMessages.clear();
    }
    if (gSelectedSavedConversationId == conversationId)
        gSelectedSavedConversationId = -1;

    LoadChatConversations();
    statusMessage = "Conversation deleted successfully.";
    return true;
}

static bool SaveSummaryToDatabase(const DocumentSummary& item)
{
    if (!gDatabase)
    {
        statusMessage = "Database is not available. Summary was not saved.";
        return false;
    }

    const char* sql =
        "INSERT INTO summaries "
        "(filename, summary_type, summary_text, generated_at) "
        "VALUES (?, ?, ?, ?);";

    sqlite3_stmt* statement = nullptr;

    if (sqlite3_prepare_v2(
        gDatabase,
        sql,
        -1,
        &statement,
        nullptr
    ) != SQLITE_OK)
    {
        statusMessage = sqlite3_errmsg(gDatabase);
        return false;
    }

    sqlite3_bind_text(
        statement, 1, item.filename.c_str(),
        -1, SQLITE_TRANSIENT
    );
    sqlite3_bind_text(
        statement, 2, item.summaryType.c_str(),
        -1, SQLITE_TRANSIENT
    );
    sqlite3_bind_text(
        statement, 3, item.text.c_str(),
        -1, SQLITE_TRANSIENT
    );
    sqlite3_bind_text(
        statement, 4, item.generatedAt.c_str(),
        -1, SQLITE_TRANSIENT
    );

    bool success = sqlite3_step(statement) == SQLITE_DONE;
    sqlite3_finalize(statement);

    if (!success)
    {
        statusMessage = sqlite3_errmsg(gDatabase);
        return false;
    }

    gSelectedSummaryId =
        static_cast<int>(sqlite3_last_insert_rowid(gDatabase));

    LoadSummaries();
    return true;
}

static bool DeleteSummaryFromDatabase(int summaryId)
{
    if (!gDatabase)
    {
        statusMessage = "Database is not available.";
        return false;
    }

    sqlite3_stmt* statement = nullptr;

    if (sqlite3_prepare_v2(
        gDatabase,
        "DELETE FROM summaries WHERE id = ?;",
        -1,
        &statement,
        nullptr
    ) != SQLITE_OK)
    {
        statusMessage = sqlite3_errmsg(gDatabase);
        return false;
    }

    sqlite3_bind_int(statement, 1, summaryId);

    bool success = sqlite3_step(statement) == SQLITE_DONE;
    sqlite3_finalize(statement);

    if (!success)
    {
        statusMessage = sqlite3_errmsg(gDatabase);
        return false;
    }

    if (gSelectedSummaryId == summaryId)
        gSelectedSummaryId = -1;

    LoadSummaries();
    statusMessage = "Summary deleted successfully.";
    return true;
}

static std::string GetExtension(const fs::path& path)
{
    std::string extension = path.extension().string();

    std::transform(
        extension.begin(),
        extension.end(),
        extension.begin(),
        [](unsigned char c)
        {
            return static_cast<char>(std::tolower(c));
        }
    );

    return extension;
}

static bool IsSupportedFile(const fs::path& path)
{
    std::string extension = GetExtension(path);

    return extension == ".pdf" ||
           extension == ".docx" ||
           extension == ".txt";
}

static bool SaveDocument(const fs::path& source)
{
    if (!gDatabase)
        return false;

    if (!fs::exists(source) || !fs::is_regular_file(source))
    {
        statusMessage = "Selected file does not exist.";
        return false;
    }

    if (!IsSupportedFile(source))
    {
        statusMessage = "Only PDF, DOCX and TXT files are supported.";
        return false;
    }

    std::string originalName = source.filename().string();
    std::string extension = GetExtension(source);

    long long fileSize =
        static_cast<long long>(fs::file_size(source));

    std::string date = GetCurrentDateTime();

    fs::path destinationDirectory =
        gProjectRoot / "documents";

    fs::create_directories(destinationDirectory);

    static int fileCounter = 0;
    fileCounter++;

    std::time_t timestamp = std::time(nullptr);

    std::string storedName =
        "document_" +
        std::to_string(timestamp) + "_" +
        std::to_string(fileCounter) +
        extension;

    fs::path destination =
        destinationDirectory / storedName;

    try
    {
        fs::copy_file(
            source,
            destination,
            fs::copy_options::none
        );
    }
    catch (const fs::filesystem_error& error)
    {
        statusMessage =
            std::string("File copy failed: ") + error.what();

        return false;
    }

    fs::path relativePath =
        fs::relative(destination, gProjectRoot);

    const char* sql =
        "INSERT INTO documents "
        "(name, path, type, size, added_at, category_id) "
        "VALUES (?, ?, ?, ?, ?, ?);";

    sqlite3_stmt* statement = nullptr;

    if (sqlite3_prepare_v2(
        gDatabase,
        sql,
        -1,
        &statement,
        nullptr
    ) != SQLITE_OK)
    {
        statusMessage = sqlite3_errmsg(gDatabase);

        fs::remove(destination);
        return false;
    }

    sqlite3_bind_text(
        statement, 1, originalName.c_str(),
        -1, SQLITE_TRANSIENT
    );

    sqlite3_bind_text(
        statement, 2, relativePath.string().c_str(),
        -1, SQLITE_TRANSIENT
    );

    sqlite3_bind_text(
        statement, 3, extension.c_str(),
        -1, SQLITE_TRANSIENT
    );

    sqlite3_bind_int64(
        statement, 4, fileSize
    );

    sqlite3_bind_text(
        statement, 5, date.c_str(),
        -1, SQLITE_TRANSIENT
    );
    sqlite3_bind_int(statement, 6, GetUncategorizedCategoryId());

    bool success =
        sqlite3_step(statement) == SQLITE_DONE;

    sqlite3_finalize(statement);

    if (!success)
    {
        statusMessage = sqlite3_errmsg(gDatabase);
        fs::remove(destination);
        return false;
    }

    LoadDocuments();

    statusMessage =
        "Successfully uploaded: " + originalName;

    return true;
}

static bool SaveTextReport(
    const std::string& dialogTitle,
    const std::string& suggestedFilename,
    const std::string& reportText)
{
    if (reportText.empty())
    {
        statusMessage = "There is no report to save yet.";
        return false;
    }

#ifdef _WIN32
    char fileName[MAX_PATH] = "";
    std::snprintf(fileName, sizeof(fileName), "%s", suggestedFilename.c_str());

    OPENFILENAMEA dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = nullptr;
    dialog.lpstrFile = fileName;
    dialog.nMaxFile = sizeof(fileName);
    dialog.lpstrFilter =
        "Text Files (*.txt)\0*.txt\0"
        "All Files (*.*)\0*.*\0";
    dialog.nFilterIndex = 1;
    dialog.lpstrDefExt = "txt";
    dialog.lpstrTitle = dialogTitle.c_str();
    dialog.Flags =
        OFN_OVERWRITEPROMPT |
        OFN_PATHMUSTEXIST |
        OFN_NOCHANGEDIR;

    if (!GetSaveFileNameA(&dialog))
    {
        DWORD error = CommDlgExtendedError();
        if (error != 0)
            statusMessage = "The save dialog encountered an error.";
        else
            statusMessage = "Save cancelled.";
        return false;
    }

    std::ofstream output(fs::path(fileName), std::ios::binary | std::ios::trunc);
    if (!output)
    {
        statusMessage = "Could not create the report file. Check the folder permissions.";
        return false;
    }

    output.write(reportText.data(), static_cast<std::streamsize>(reportText.size()));
    output.flush();
    if (!output.good())
    {
        statusMessage = "The report could not be written completely.";
        return false;
    }

    statusMessage = "Report saved successfully: " + std::string(fileName);
    return true;
#else
    (void)dialogTitle;
    (void)suggestedFilename;
    statusMessage = "Report saving is currently available on Windows.";
    return false;
#endif
}


static std::string PdfSafeText(const std::string& input)
{
    std::string output;
    output.reserve(input.size());

    for (unsigned char ch : input)
    {
        if (ch == '\r')
            continue;
        if (ch == '\t')
        {
            output += "    ";
            continue;
        }
        if (ch == '\n' || (ch >= 32 && ch <= 126))
            output += static_cast<char>(ch);
        else if ((ch & 0xC0) != 0x80)
            output += '?';
    }

    return output;
}

static std::string EscapePdfString(const std::string& input)
{
    std::string output;
    output.reserve(input.size());
    for (char ch : input)
    {
        if (ch == '(' || ch == ')' || ch == '\\')
            output += '\\';
        if (ch == '\n' || ch == '\r')
            output += ' ';
        else
            output += ch;
    }
    return output;
}

static std::vector<std::string> WrapPdfReportText(const std::string& reportText)
{
    const std::string safeText = PdfSafeText(reportText);
    std::vector<std::string> lines;
    std::istringstream input(safeText);
    std::string paragraph;

    while (std::getline(input, paragraph))
    {
        if (paragraph.empty())
        {
            lines.emplace_back();
            continue;
        }

        const size_t maxLineLength = 92;
        while (paragraph.size() > maxLineLength)
        {
            size_t splitAt = paragraph.rfind(' ', maxLineLength);
            if (splitAt == std::string::npos || splitAt == 0)
                splitAt = maxLineLength;

            lines.push_back(paragraph.substr(0, splitAt));
            paragraph.erase(0, splitAt);
            while (!paragraph.empty() && paragraph.front() == ' ')
                paragraph.erase(paragraph.begin());
        }
        lines.push_back(paragraph);
    }

    if (lines.empty())
        lines.emplace_back("No report content was provided.");

    return lines;
}

static bool WritePdfReportFile(
    const fs::path& destination,
    const std::string& reportTitle,
    const std::string& reportText)
{
    const std::vector<std::string> wrappedLines = WrapPdfReportText(reportText);
    const size_t linesPerPage = 47;
    const size_t pageCount = (wrappedLines.size() + linesPerPage - 1) / linesPerPage;

    std::vector<std::string> objects;
    objects.resize(3 + pageCount * 2);
    objects[0] = "<< /Type /Catalog /Pages 2 0 R >>";
    objects[2] = "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>";

    std::ostringstream kids;
    kids << "[";

    for (size_t page = 0; page < pageCount; ++page)
    {
        const int pageObjectNumber = static_cast<int>(4 + page * 2);
        const int contentObjectNumber = pageObjectNumber + 1;
        kids << pageObjectNumber << " 0 R ";

        std::ostringstream content;
        content << "BT /F1 15 Tf 54 750 Td (DocuMind AI Report) Tj ET\n";
        content << "BT /F1 10 Tf 54 733 Td (" << EscapePdfString(PdfSafeText(reportTitle)) << ") Tj ET\n";
        content << "0.65 w 54 722 m 558 722 l S\n";

        float y = 704.0f;
        const size_t begin = page * linesPerPage;
        const size_t end = std::min(begin + linesPerPage, wrappedLines.size());
        for (size_t line = begin; line < end; ++line)
        {
            content << "BT /F1 9 Tf 54 " << y << " Td ("
                    << EscapePdfString(wrappedLines[line])
                    << ") Tj ET\n";
            y -= 14.0f;
        }

        content << "BT /F1 8 Tf 500 30 Td (Page " << (page + 1)
                << " of " << pageCount << ") Tj ET\n";

        const std::string stream = content.str();
        objects[pageObjectNumber - 1] =
            "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
            "/Resources << /Font << /F1 3 0 R >> >> /Contents " +
            std::to_string(contentObjectNumber) + " 0 R >>";
        objects[contentObjectNumber - 1] =
            "<< /Length " + std::to_string(stream.size()) +
            " >>\nstream\n" + stream + "endstream";
    }

    kids << "]";
    objects[1] = "<< /Type /Pages /Kids " + kids.str() +
                 " /Count " + std::to_string(pageCount) + " >>";

    std::ofstream output(destination, std::ios::binary | std::ios::trunc);
    if (!output)
        return false;

    output << "%PDF-1.4\n%DocuMind\n";
    std::vector<std::streamoff> offsets(objects.size() + 1, 0);
    for (size_t i = 0; i < objects.size(); ++i)
    {
        offsets[i + 1] = output.tellp();
        output << (i + 1) << " 0 obj\n" << objects[i] << "\nendobj\n";
    }

    const std::streamoff xrefOffset = output.tellp();
    output << "xref\n0 " << (objects.size() + 1) << "\n";
    output << "0000000000 65535 f \n";
    for (size_t i = 1; i < offsets.size(); ++i)
    {
        output << std::setw(10) << std::setfill('0') << offsets[i]
               << " 00000 n \n";
    }
    output << "trailer\n<< /Size " << (objects.size() + 1)
           << " /Root 1 0 R >>\nstartxref\n"
           << xrefOffset << "\n%%EOF\n";
    output.flush();
    return output.good();
}

static bool SavePdfReport(
    const std::string& dialogTitle,
    const std::string& suggestedFilename,
    const std::string& reportTitle,
    const std::string& reportText)
{
    if (reportText.empty())
    {
        statusMessage = "There is no report to export yet.";
        return false;
    }

#ifdef _WIN32
    char fileName[MAX_PATH] = "";
    std::snprintf(fileName, sizeof(fileName), "%s", suggestedFilename.c_str());

    OPENFILENAMEA dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = nullptr;
    dialog.lpstrFile = fileName;
    dialog.nMaxFile = sizeof(fileName);
    dialog.lpstrFilter =
        "PDF Files (*.pdf)\0*.pdf\0"
        "All Files (*.*)\0*.*\0";
    dialog.nFilterIndex = 1;
    dialog.lpstrDefExt = "pdf";
    dialog.lpstrTitle = dialogTitle.c_str();
    dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    if (!GetSaveFileNameA(&dialog))
    {
        DWORD error = CommDlgExtendedError();
        statusMessage = error != 0
            ? "The PDF save dialog encountered an error."
            : "PDF export cancelled.";
        return false;
    }

    if (!WritePdfReportFile(fs::path(fileName), reportTitle, reportText))
    {
        statusMessage = "Could not create the PDF report. Check the folder permissions.";
        return false;
    }

    statusMessage = "PDF report saved successfully: " + std::string(fileName);
    return true;
#else
    (void)dialogTitle;
    (void)suggestedFilename;
    (void)reportTitle;
    statusMessage = "PDF report export is currently available on Windows.";
    return false;
#endif
}


static std::string EscapeCsvField(const std::string& value)
{
    std::string escaped = value;
    size_t position = 0;
    while ((position = escaped.find('"', position)) != std::string::npos)
    {
        escaped.insert(position, 1, '"');
        position += 2;
    }
    if (escaped.find_first_of(",\"\r\n") != std::string::npos)
        return "\"" + escaped + "\"";
    return escaped;
}

static bool SaveCsvReport(
    const std::string& dialogTitle,
    const std::string& suggestedFilename,
    const std::string& csvContent)
{
    if (csvContent.empty())
    {
        statusMessage = "There is no data available to export.";
        return false;
    }
#ifdef _WIN32
    char fileName[MAX_PATH] = "";
    std::snprintf(fileName, sizeof(fileName), "%s", suggestedFilename.c_str());
    OPENFILENAMEA dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = nullptr;
    dialog.lpstrFile = fileName;
    dialog.nMaxFile = sizeof(fileName);
    dialog.lpstrFilter = "CSV Files (*.csv)\0*.csv\0All Files (*.*)\0*.*\0";
    dialog.nFilterIndex = 1;
    dialog.lpstrDefExt = "csv";
    dialog.lpstrTitle = dialogTitle.c_str();
    dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameA(&dialog))
    {
        DWORD error = CommDlgExtendedError();
        statusMessage = error != 0 ? "The CSV save dialog encountered an error." : "CSV export cancelled.";
        return false;
    }
    std::ofstream output(fs::path(fileName), std::ios::binary | std::ios::trunc);
    if (!output)
    {
        statusMessage = "Could not create the CSV file. Check folder permissions.";
        return false;
    }
    // UTF-8 BOM helps spreadsheet applications detect UTF-8 correctly.
    output.write("\xEF\xBB\xBF", 3);
    output.write(csvContent.data(), static_cast<std::streamsize>(csvContent.size()));
    output.flush();
    if (!output.good())
    {
        statusMessage = "The CSV file could not be written completely.";
        return false;
    }
    statusMessage = "CSV exported successfully: " + std::string(fileName);
    return true;
#else
    (void)dialogTitle;
    (void)suggestedFilename;
    statusMessage = "CSV export is currently available on Windows.";
    return false;
#endif
}

static std::string BuildDocumentInventoryCsv()
{
    if (gDocuments.empty()) return {};
    std::ostringstream csv;
    csv << "Document Name,File Type,Size (Bytes),Added At,Category,Tags,Stored Path\r\n";
    for (const Document& document : gDocuments)
    {
        std::string tags;
        for (size_t i = 0; i < document.tags.size(); ++i)
        {
            if (i) tags += "; ";
            tags += document.tags[i];
        }
        csv << EscapeCsvField(document.name) << ','
            << EscapeCsvField(document.type) << ','
            << document.size << ','
            << EscapeCsvField(document.addedAt) << ','
            << EscapeCsvField(document.categoryName) << ','
            << EscapeCsvField(tags) << ','
            << EscapeCsvField(document.path) << "\r\n";
    }
    return csv.str();
}

static std::string BuildSummaryHistoryCsv()
{
    if (gSummaries.empty()) return {};
    std::ostringstream csv;
    csv << "Document Name,Summary Type,Generated At,Summary Text\r\n";
    for (const DocumentSummary& summary : gSummaries)
    {
        csv << EscapeCsvField(summary.filename) << ','
            << EscapeCsvField(summary.summaryType) << ','
            << EscapeCsvField(summary.generatedAt) << ','
            << EscapeCsvField(summary.text) << "\r\n";
    }
    return csv.str();
}

static void OpenDocumentPicker()
{
#ifdef _WIN32

    char fileName[MAX_PATH] = "";

    OPENFILENAMEA dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = nullptr;
    dialog.lpstrFile = fileName;
    dialog.nMaxFile = sizeof(fileName);

    dialog.lpstrFilter =
        "Supported Documents (*.pdf;*.docx;*.txt)\0"
        "*.pdf;*.docx;*.txt\0"
        "PDF Files (*.pdf)\0*.pdf\0"
        "Word Documents (*.docx)\0*.docx\0"
        "Text Files (*.txt)\0*.txt\0"
        "All Files (*.*)\0*.*\0";

    dialog.nFilterIndex = 1;

    dialog.Flags =
        OFN_FILEMUSTEXIST |
        OFN_PATHMUSTEXIST |
        OFN_NOCHANGEDIR;

    dialog.lpstrTitle = "Select a document for DocuMind AI";

    if (GetOpenFileNameA(&dialog))
    {
        try
        {
            SaveDocument(fs::path(fileName));
        }
        catch (const std::exception& error)
        {
            statusMessage =
                std::string("Upload error: ") + error.what();
        }
    }
    else
    {
        DWORD error = CommDlgExtendedError();

        if (error != 0)
            statusMessage = "The file picker encountered an error.";
    }

#else

    statusMessage =
        "The document picker is currently available on Windows.";

#endif
}

static long long GetTotalStorage()
{
    long long total = 0;

    for (const Document& document : gDocuments)
        total += document.size;

    return total;
}


// FNV-1a is used as a fast grouping fingerprint. Matching files are also
// compared byte-for-byte before they are reported as duplicates.
static bool CalculateFileFingerprint(const fs::path& path, std::uint64_t& fingerprint)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return false;

    std::uint64_t hash = 14695981039346656037ULL;
    char buffer[8192];

    while (input)
    {
        input.read(buffer, sizeof(buffer));
        const std::streamsize count = input.gcount();

        for (std::streamsize i = 0; i < count; ++i)
        {
            hash ^= static_cast<unsigned char>(buffer[i]);
            hash *= 1099511628211ULL;
        }
    }

    if (!input.eof())
        return false;

    fingerprint = hash;
    return true;
}

static bool FilesHaveIdenticalContents(const fs::path& firstPath, const fs::path& secondPath)
{
    std::ifstream first(firstPath, std::ios::binary);
    std::ifstream second(secondPath, std::ios::binary);

    if (!first || !second)
        return false;

    char firstBuffer[8192];
    char secondBuffer[8192];

    while (true)
    {
        first.read(firstBuffer, sizeof(firstBuffer));
        second.read(secondBuffer, sizeof(secondBuffer));

        const std::streamsize firstCount = first.gcount();
        const std::streamsize secondCount = second.gcount();

        if (firstCount != secondCount)
            return false;

        if (firstCount > 0 &&
            std::memcmp(firstBuffer, secondBuffer, static_cast<size_t>(firstCount)) != 0)
            return false;

        if (first.eof() && second.eof())
            return true;

        if (first.bad() || second.bad())
            return false;
    }
}

static fs::path ResolveStoredDocumentPath(const Document& document)
{
    fs::path storedPath(document.path);
    return storedPath.is_absolute() ? storedPath : (gProjectRoot / storedPath);
}

static void ScanForDuplicateDocuments()
{
    gDuplicateGroups.clear();
    gDuplicateFilesChecked = 0;
    gDuplicateMissingFiles = 0;

    struct FingerprintedDocument
    {
        Document document;
        fs::path path;
        std::uint64_t fingerprint = 0;
    };

    std::vector<FingerprintedDocument> indexedFiles;

    for (const Document& document : gDocuments)
    {
        const fs::path path = ResolveStoredDocumentPath(document);

        try
        {
            if (!fs::exists(path) || !fs::is_regular_file(path))
            {
                ++gDuplicateMissingFiles;
                continue;
            }

            std::uint64_t fingerprint = 0;
            if (!CalculateFileFingerprint(path, fingerprint))
            {
                ++gDuplicateMissingFiles;
                continue;
            }

            indexedFiles.push_back({document, path, fingerprint});
            ++gDuplicateFilesChecked;
        }
        catch (const fs::filesystem_error&)
        {
            ++gDuplicateMissingFiles;
        }
    }

    std::vector<bool> assigned(indexedFiles.size(), false);

    for (size_t i = 0; i < indexedFiles.size(); ++i)
    {
        if (assigned[i])
            continue;

        DuplicateDocumentGroup group;
        group.documents.push_back(indexedFiles[i].document);

        for (size_t j = i + 1; j < indexedFiles.size(); ++j)
        {
            if (assigned[j])
                continue;

            if (indexedFiles[i].document.size != indexedFiles[j].document.size ||
                indexedFiles[i].fingerprint != indexedFiles[j].fingerprint)
                continue;

            if (FilesHaveIdenticalContents(indexedFiles[i].path, indexedFiles[j].path))
            {
                group.documents.push_back(indexedFiles[j].document);
                assigned[j] = true;
            }
        }

        if (group.documents.size() > 1)
        {
            assigned[i] = true;
            gDuplicateGroups.push_back(std::move(group));
        }
    }

    gDuplicateScanCompleted = true;

    int duplicateCount = 0;
    for (const DuplicateDocumentGroup& group : gDuplicateGroups)
        duplicateCount += static_cast<int>(group.documents.size());

    statusMessage = "Duplicate scan complete: " +
        std::to_string(gDuplicateGroups.size()) + " duplicate groups found (" +
        std::to_string(duplicateCount) + " files).";
    if (gDuplicateMissingFiles > 0)
        statusMessage += " " + std::to_string(gDuplicateMissingFiles) +
            " missing or unreadable files were skipped.";
}

#ifdef _WIN32

static void AppendUtf8(
    std::string& output,
    unsigned int codePoint)
{
    if (codePoint <= 0x7F)
    {
        output += static_cast<char>(codePoint);
    }
    else if (codePoint <= 0x7FF)
    {
        output += static_cast<char>(0xC0 | (codePoint >> 6));
        output += static_cast<char>(0x80 | (codePoint & 0x3F));
    }
    else if (codePoint <= 0xFFFF)
    {
        output += static_cast<char>(0xE0 | (codePoint >> 12));
        output += static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F));
        output += static_cast<char>(0x80 | (codePoint & 0x3F));
    }
    else
    {
        output += static_cast<char>(0xF0 | (codePoint >> 18));
        output += static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F));
        output += static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F));
        output += static_cast<char>(0x80 | (codePoint & 0x3F));
    }
}

static int HexValue(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';

    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;

    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;

    return -1;
}

static std::string GetJsonStringValue(
    const std::string& json,
    const std::string& key)
{
    std::string search = "\"" + key + "\"";

    size_t position = json.find(search);

    if (position == std::string::npos)
        return "";

    position = json.find(':', position + search.size());

    if (position == std::string::npos)
        return "";

    position = json.find('"', position + 1);

    if (position == std::string::npos)
        return "";

    position++;

    std::string result;

    while (position < json.size())
    {
        unsigned char c =
            static_cast<unsigned char>(json[position++]);

        if (c == '"')
            return result;

        if (c != '\\')
        {
            result += static_cast<char>(c);
            continue;
        }

        if (position >= json.size())
            break;

        char escaped = json[position++];

        switch (escaped)
        {
        case '"': result += '"'; break;
        case '\\': result += '\\'; break;
        case '/': result += '/'; break;
        case 'n': result += '\n'; break;
        case 'r': result += '\r'; break;
        case 't': result += '\t'; break;
        case 'b': result += '\b'; break;
        case 'f': result += '\f'; break;

        case 'u':
        {
            if (position + 4 > json.size())
                return result;

            unsigned int codePoint = 0;

            for (int i = 0; i < 4; i++)
            {
                int value = HexValue(json[position++]);

                if (value < 0)
                    return result;

                codePoint =
                    (codePoint << 4) |
                    static_cast<unsigned int>(value);
            }

            if (codePoint >= 0xD800 && codePoint <= 0xDBFF &&
                position + 6 <= json.size() &&
                json[position] == '\\' &&
                json[position + 1] == 'u')
            {
                position += 2;

                unsigned int low = 0;

                bool valid = true;

                for (int i = 0; i < 4; i++)
                {
                    int value = HexValue(json[position++]);

                    if (value < 0)
                    {
                        valid = false;
                        break;
                    }

                    low =
                        (low << 4) |
                        static_cast<unsigned int>(value);
                }

                if (valid && low >= 0xDC00 && low <= 0xDFFF)
                {
                    codePoint =
                        0x10000 +
                        ((codePoint - 0xD800) << 10) +
                        (low - 0xDC00);
                }
            }

            AppendUtf8(result, codePoint);
            break;
        }

        default:
            result += escaped;
            break;
        }
    }

    return result;
}

static bool ExtractDocumentText(const Document& document)
{
    fs::path filePath =
        gProjectRoot / fs::path(document.path);

    if (!fs::exists(filePath))
    {
        statusMessage = "Document file could not be found.";
        return false;
    }

    std::ifstream file(filePath, std::ios::binary);

    if (!file)
    {
        statusMessage = "Could not read the selected document.";
        return false;
    }

    std::vector<char> fileData(
        (std::istreambuf_iterator<char>(file)),
        std::istreambuf_iterator<char>()
    );

    if (fileData.empty())
    {
        statusMessage = "The selected document is empty.";
        return false;
    }

    if (fileData.size() > 20 * 1024 * 1024)
    {
        statusMessage = "Document exceeds the 20 MB extraction limit.";
        return false;
    }

    std::string boundary =
        "----DocuMindBoundary7MA4YWxkTrZu0gW";

    std::string filename =
        "document" + GetExtension(filePath);

    std::string multipartHeader =
        "--" + boundary + "\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"" +
        filename + "\"\r\n"
        "Content-Type: application/octet-stream\r\n\r\n";

    std::vector<char> requestBody;

    requestBody.insert(
        requestBody.end(),
        multipartHeader.begin(),
        multipartHeader.end()
    );

    requestBody.insert(
        requestBody.end(),
        fileData.begin(),
        fileData.end()
    );

    std::string ending =
        "\r\n--" + boundary + "--\r\n";

    requestBody.insert(
        requestBody.end(),
        ending.begin(),
        ending.end()
    );

    HINTERNET session = WinHttpOpen(
    L"DocuMindAI/1.0",
    WINHTTP_ACCESS_TYPE_NO_PROXY,
    WINHTTP_NO_PROXY_NAME,
    WINHTTP_NO_PROXY_BYPASS,
    0
);

    if (!session)
    {
        statusMessage = "Could not initialize Windows HTTP.";
        return false;
    }

    HINTERNET connection = WinHttpConnect(
    session,
    L"127.0.0.1",
    8000,
    0
);

    if (!connection)
    {
        statusMessage = "Could not connect to the Python AI service.";
        WinHttpCloseHandle(session);
        return false;
    }

    HINTERNET request = WinHttpOpenRequest(
        connection,
        L"POST",
        L"/extract",
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        0
    );

    if (!request)
    {
        statusMessage = "Could not create the extraction request.";
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return false;
    }

    std::wstring wideBoundary(
        boundary.begin(),
        boundary.end()
    );

    std::wstring headers =
        L"Content-Type: multipart/form-data; boundary=" +
        wideBoundary;

    BOOL sent = WinHttpSendRequest(
        request,
        headers.c_str(),
        static_cast<DWORD>(headers.size()),
        requestBody.data(),
        static_cast<DWORD>(requestBody.size()),
        static_cast<DWORD>(requestBody.size()),
        0
    );

    if (!sent)
{
    DWORD error = GetLastError();

    statusMessage =
        "Failed to send request to Python AI service. "
        "Windows error: " + std::to_string(error);

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);

    return false;
}

if (!WinHttpReceiveResponse(request, nullptr))
{
    DWORD error = GetLastError();

    statusMessage =
        "Python AI response failed. "
        "Windows error: " + std::to_string(error);

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);

    return false;
}

    DWORD httpStatus = 0;
    DWORD statusSize = sizeof(httpStatus);

    WinHttpQueryHeaders(
        request,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX,
        &httpStatus,
        &statusSize,
        WINHTTP_NO_HEADER_INDEX
    );

    std::string response;

    DWORD available = 0;

    while (WinHttpQueryDataAvailable(request, &available) &&
           available > 0)
    {
        std::vector<char> buffer(available);

        DWORD downloaded = 0;

        if (!WinHttpReadData(
            request,
            buffer.data(),
            available,
            &downloaded))
        {
            break;
        }

        response.append(buffer.data(), downloaded);
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);

    if (httpStatus != 200)
    {
        std::string detail =
            GetJsonStringValue(response, "detail");

        statusMessage =
            "Extraction failed (HTTP " +
            std::to_string(httpStatus) + "): " + detail;

        return false;
    }

    std::string extractedText =
        GetJsonStringValue(response, "text");

    if (extractedText.empty())
    {
        statusMessage =
            "No readable text was found in this document.";

        return false;
    }

    gExtractedDocumentText = std::move(extractedText);
    gExtractedDocumentName = document.name;

    gOpenReaderPopup = true;

    statusMessage =
        "Successfully extracted text from: " + document.name;

    return true;
}

static std::string EscapeJsonString(const std::string& value)
{
    std::string escaped;
    escaped.reserve(value.size() + 32);

    for (unsigned char c : value)
    {
        switch (c)
        {
        case '"': escaped += "\\\""; break;
        case '\\': escaped += "\\\\"; break;
        case '\b': escaped += "\\b"; break;
        case '\f': escaped += "\\f"; break;
        case '\n': escaped += "\\n"; break;
        case '\r': escaped += "\\r"; break;
        case '\t': escaped += "\\t"; break;
        default:
            if (c < 0x20)
            {
                char buffer[7];
                std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
                escaped += buffer;
            }
            else
            {
                escaped += static_cast<char>(c);
            }
            break;
        }
    }

    return escaped;
}

static bool SummarizeDocumentText(
    const std::string& filename,
    const std::string& documentText,
    const std::string& summaryType)
{
    if (documentText.empty())
    {
        statusMessage = "There is no extracted document text to summarize.";
        return false;
    }

    if (documentText.size() > 120000)
    {
        statusMessage =
            "This document is too large for one summary request. "
            "Long-document processing will be added separately.";
        return false;
    }

    if (summaryType != "short" && summaryType != "detailed")
    {
        statusMessage = "Invalid summary type.";
        return false;
    }

    std::string requestBody =
        "{\"filename\":\"" + EscapeJsonString(filename) +
        "\",\"text\":\"" + EscapeJsonString(documentText) +
        "\",\"summary_type\":\"" + summaryType + "\"}";

    HINTERNET session = WinHttpOpen(
        L"DocuMindAI/1.0",
        WINHTTP_ACCESS_TYPE_NO_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0
    );

    if (!session)
    {
        statusMessage = "Could not initialize Windows HTTP for summarization.";
        return false;
    }

    WinHttpSetTimeouts(session, 5000, 5000, 300000, 300000);

    HINTERNET connection = WinHttpConnect(
        session,
        L"127.0.0.1",
        8000,
        0
    );

    if (!connection)
    {
        DWORD error = GetLastError();
        statusMessage =
            "Could not connect to DocuMind Python service. Windows error: " +
            std::to_string(error);
        WinHttpCloseHandle(session);
        return false;
    }

    HINTERNET request = WinHttpOpenRequest(
        connection,
        L"POST",
        L"/summarize",
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        0
    );

    if (!request)
    {
        DWORD error = GetLastError();
        statusMessage =
            "Could not create the summary request. Windows error: " +
            std::to_string(error);
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return false;
    }

    const wchar_t* headers = L"Content-Type: application/json\r\n";

    BOOL sent = WinHttpSendRequest(
        request,
        headers,
        static_cast<DWORD>(-1L),
        const_cast<char*>(requestBody.data()),
        static_cast<DWORD>(requestBody.size()),
        static_cast<DWORD>(requestBody.size()),
        0
    );

    if (!sent || !WinHttpReceiveResponse(request, nullptr))
    {
        DWORD error = GetLastError();
        statusMessage =
            "The local AI summary request failed. Windows error: " +
            std::to_string(error);
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return false;
    }

    DWORD httpStatus = 0;
    DWORD statusSize = sizeof(httpStatus);

    WinHttpQueryHeaders(
        request,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX,
        &httpStatus,
        &statusSize,
        WINHTTP_NO_HEADER_INDEX
    );

    std::string responseBody;
    DWORD available = 0;

    while (WinHttpQueryDataAvailable(request, &available) && available > 0)
    {
        std::vector<char> buffer(available);
        DWORD downloaded = 0;

        if (!WinHttpReadData(request, buffer.data(), available, &downloaded))
            break;

        responseBody.append(buffer.data(), downloaded);
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);

    if (httpStatus != 200)
    {
        std::string detail = GetJsonStringValue(responseBody, "detail");
        statusMessage =
            "Summarization failed (HTTP " + std::to_string(httpStatus) +"): " +
            (detail.empty() ? responseBody : detail);
        return false;
    }

    std::string summary = GetJsonStringValue(responseBody, "summary");

    if (summary.empty())
    {
        statusMessage = "The local AI returned an empty summary.";
        return false;
    }

    DocumentSummary item;
    item.filename = filename;
    item.summaryType = summaryType;
    item.text = std::move(summary);
    item.generatedAt = GetCurrentDateTime();

    if (!SaveSummaryToDatabase(item))
    {
        statusMessage =
            "Summary was generated, but could not be saved: " +
            statusMessage;
        return false;
    }

    statusMessage =
        "Successfully generated and saved a " + summaryType +
        " summary for: " + filename;

    return true;
}

static bool AskDocumentQuestion(
    const std::string& filename,
    const std::string& documentText,
    const std::string& question)
{
    if (documentText.empty() || question.empty())
    {
        statusMessage = "Load a document and enter a question first.";
        return false;
    }

    std::string requestBody =
        "{\"filename\":\"" + EscapeJsonString(filename) +
        "\",\"text\":\"" + EscapeJsonString(documentText) +
        "\",\"question\":\"" + EscapeJsonString(question) +
        "\",\"history\":[";

    int firstHistoryIndex = std::max(
        0,
        static_cast<int>(gChatMessages.size()) - 10
    );

    for (int i = firstHistoryIndex; i < static_cast<int>(gChatMessages.size()); i++)
    {
        if (i > firstHistoryIndex)
            requestBody += ",";

        requestBody +=
            "{\"role\":\"" + EscapeJsonString(gChatMessages[i].role) +
            "\",\"content\":\"" + EscapeJsonString(gChatMessages[i].content) + "\"}";
    }

    requestBody += "]}";

    HINTERNET session = WinHttpOpen(
        L"DocuMindAI/1.0",
        WINHTTP_ACCESS_TYPE_NO_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0
    );

    if (!session)
    {
        statusMessage = "Could not initialize Windows HTTP for AI chat.";
        return false;
    }

    WinHttpSetTimeouts(session, 5000, 5000, 300000, 300000);

    HINTERNET connection = WinHttpConnect(
        session,
        L"127.0.0.1",
        8000,
        0
    );

    if (!connection)
    {
        DWORD error = GetLastError();
        statusMessage =
            "Could not connect to DocuMind Python service. Windows error: " +
            std::to_string(error);
        WinHttpCloseHandle(session);
        return false;
    }

    HINTERNET request = WinHttpOpenRequest(
        connection,
        L"POST",
        L"/chat",
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        0
    );

    if (!request)
    {
        DWORD error = GetLastError();
        statusMessage =
            "Could not create the AI chat request. Windows error: " +
            std::to_string(error);
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return false;
    }

    const wchar_t* headers = L"Content-Type: application/json\r\n";

    BOOL sent = WinHttpSendRequest(
        request,
        headers,
        static_cast<DWORD>(-1L),
        const_cast<char*>(requestBody.data()),
        static_cast<DWORD>(requestBody.size()),
        static_cast<DWORD>(requestBody.size()),
        0
    );

    if (!sent || !WinHttpReceiveResponse(request, nullptr))
    {
        DWORD error = GetLastError();
        statusMessage =
            "The local AI chat request failed. Windows error: " +
            std::to_string(error);
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return false;
    }

    DWORD httpStatus = 0;
    DWORD statusSize = sizeof(httpStatus);

    WinHttpQueryHeaders(
        request,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX,
        &httpStatus,
        &statusSize,
        WINHTTP_NO_HEADER_INDEX
    );

    std::string responseBody;
    DWORD available = 0;

    while (WinHttpQueryDataAvailable(request, &available) && available > 0)
    {
        std::vector<char> buffer(available);
        DWORD downloaded = 0;

        if (!WinHttpReadData(request, buffer.data(), available, &downloaded))
            break;

        responseBody.append(buffer.data(), downloaded);
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);

    if (httpStatus != 200)
    {
        std::string detail = GetJsonStringValue(responseBody, "detail");
        statusMessage =
            "AI chat failed (HTTP " + std::to_string(httpStatus) + "): " +
            (detail.empty() ? responseBody : detail);
        return false;
    }

    std::string answer = GetJsonStringValue(responseBody, "answer");
    if (answer.empty())
    {
        statusMessage = "The local AI returned an empty answer.";
        return false;
    }

    bool savedToHistory = SaveChatTurn(
        gSelectedChatDocumentId,
        filename,
        question,
        answer
    );

    gChatMessages.push_back({ "user", question });
    gChatMessages.push_back({ "assistant", answer });

    statusMessage = savedToHistory
        ? "DocuMind AI answered your question. Conversation saved."
        : "DocuMind AI answered, but the conversation could not be saved: " + statusMessage;
    return true;
}


static bool ExtractKeyInformation(
    const std::string& filename,
    const std::string& documentText)
{
    if (documentText.empty())
    {
        statusMessage = "Load a document before extracting key information.";
        return false;
    }

    std::string requestBody =
        "{\"filename\":\"" + EscapeJsonString(filename) +
        "\",\"text\":\"" + EscapeJsonString(documentText) + "\"}";

    HINTERNET session = WinHttpOpen(
        L"DocuMindAI/1.0",
        WINHTTP_ACCESS_TYPE_NO_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0
    );

    if (!session)
    {
        statusMessage = "Could not initialize Windows HTTP for key information extraction.";
        return false;
    }

    WinHttpSetTimeouts(session, 5000, 5000, 300000, 300000);

    HINTERNET connection = WinHttpConnect(session, L"127.0.0.1", 8000, 0);
    if (!connection)
    {
        DWORD error = GetLastError();
        statusMessage = "Could not connect to DocuMind Python service. Windows error: " + std::to_string(error);
        WinHttpCloseHandle(session);
        return false;
    }

    HINTERNET request = WinHttpOpenRequest(
        connection, L"POST", L"/key-info", nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0
    );

    if (!request)
    {
        DWORD error = GetLastError();
        statusMessage = "Could not create the key information request. Windows error: " + std::to_string(error);
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return false;
    }

    const wchar_t* headers = L"Content-Type: application/json\r\n";
    BOOL sent = WinHttpSendRequest(
        request, headers, static_cast<DWORD>(-1L),
        const_cast<char*>(requestBody.data()),
        static_cast<DWORD>(requestBody.size()),
        static_cast<DWORD>(requestBody.size()), 0
    );

    if (!sent || !WinHttpReceiveResponse(request, nullptr))
    {
        DWORD error = GetLastError();
        statusMessage = "The key information request failed. Windows error: " + std::to_string(error);
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return false;
    }

    DWORD httpStatus = 0;
    DWORD statusSize = sizeof(httpStatus);
    WinHttpQueryHeaders(
        request,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX,
        &httpStatus,
        &statusSize,
        WINHTTP_NO_HEADER_INDEX
    );

    std::string responseBody;
    DWORD available = 0;
    while (WinHttpQueryDataAvailable(request, &available) && available > 0)
    {
        std::vector<char> buffer(available);
        DWORD downloaded = 0;
        if (!WinHttpReadData(request, buffer.data(), available, &downloaded))
            break;
        responseBody.append(buffer.data(), downloaded);
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);

    if (httpStatus != 200)
    {
        std::string detail = GetJsonStringValue(responseBody, "detail");
        statusMessage = "Key information extraction failed (HTTP " +
            std::to_string(httpStatus) + "): " +
            (detail.empty() ? responseBody : detail);
        return false;
    }

    std::string report = GetJsonStringValue(responseBody, "report");
    if (report.empty())
    {
        statusMessage = "The local AI returned an empty key information report.";
        return false;
    }

    gKeyInformationReport = std::move(report);
    statusMessage = "Key information extracted successfully from " + filename;
    return true;
}


static bool CompareDocumentTexts(
    const std::string& filenameA,
    const std::string& textA,
    const std::string& filenameB,
    const std::string& textB)
{
    if (textA.empty() || textB.empty())
    {
        statusMessage = "Both documents must be loaded and contain readable text.";
        return false;
    }

    if (textA.size() > 60000 || textB.size() > 60000)
    {
        statusMessage = "Each document must be 60,000 characters or fewer for comparison.";
        return false;
    }

    std::string requestBody =
        "{\\\"filename_a\\\":\\\"" + EscapeJsonString(filenameA) +
        "\\\",\\\"text_a\\\":\\\"" + EscapeJsonString(textA) +
        "\\\",\\\"filename_b\\\":\\\"" + EscapeJsonString(filenameB) +
        "\\\",\\\"text_b\\\":\\\"" + EscapeJsonString(textB) + "\\\"}";

    HINTERNET session = WinHttpOpen(
        L"DocuMindAI/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session)
    {
        statusMessage = "Could not initialize Windows HTTP for document comparison.";
        return false;
    }

    WinHttpSetTimeouts(session, 5000, 5000, 300000, 300000);
    HINTERNET connection = WinHttpConnect(session, L"127.0.0.1", 8000, 0);
    if (!connection)
    {
        DWORD error = GetLastError();
        statusMessage = "Could not connect to DocuMind Python service. Windows error: " + std::to_string(error);
        WinHttpCloseHandle(session);
        return false;
    }

    HINTERNET request = WinHttpOpenRequest(
        connection, L"POST", L"/compare", nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!request)
    {
        DWORD error = GetLastError();
        statusMessage = "Could not create the document comparison request. Windows error: " + std::to_string(error);
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return false;
    }

    const wchar_t* headers = L"Content-Type: application/json\r\n";
    BOOL sent = WinHttpSendRequest(
        request, headers, static_cast<DWORD>(-1L),
        const_cast<char*>(requestBody.data()),
        static_cast<DWORD>(requestBody.size()),
        static_cast<DWORD>(requestBody.size()), 0);

    if (!sent || !WinHttpReceiveResponse(request, nullptr))
    {
        DWORD error = GetLastError();
        statusMessage = "The document comparison request failed. Windows error: " + std::to_string(error);
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return false;
    }

    DWORD httpStatus = 0;
    DWORD statusSize = sizeof(httpStatus);
    WinHttpQueryHeaders(
        request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &httpStatus, &statusSize,
        WINHTTP_NO_HEADER_INDEX);

    std::string responseBody;
    DWORD available = 0;
    while (WinHttpQueryDataAvailable(request, &available) && available > 0)
    {
        std::vector<char> buffer(available);
        DWORD downloaded = 0;
        if (!WinHttpReadData(request, buffer.data(), available, &downloaded))
            break;
        responseBody.append(buffer.data(), downloaded);
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);

    if (httpStatus != 200)
    {
        std::string detail = GetJsonStringValue(responseBody, "detail");
        statusMessage = "Document comparison failed (HTTP " + std::to_string(httpStatus) + "): " +
            (detail.empty() ? responseBody : detail);
        return false;
    }

    std::string report = GetJsonStringValue(responseBody, "comparison");
    if (report.empty())
    {
        statusMessage = "The local AI returned an empty comparison report.";
        return false;
    }

    gDocumentComparisonReport = std::move(report);
    statusMessage = "Document comparison completed successfully.";
    return true;
}

#endif

static bool DeleteDocument(int id)
{
    if (!gDatabase)
        return false;

    std::string storedPath;

    sqlite3_stmt* findStatement = nullptr;

    if (sqlite3_prepare_v2(
        gDatabase,
        "SELECT path FROM documents WHERE id = ?;",
        -1,
        &findStatement,
        nullptr
    ) != SQLITE_OK)
    {
        statusMessage = sqlite3_errmsg(gDatabase);
        return false;
    }

    sqlite3_bind_int(findStatement, 1, id);

    if (sqlite3_step(findStatement) == SQLITE_ROW)
    {
        const unsigned char* path =
            sqlite3_column_text(findStatement, 0);

        if (path)
            storedPath =
                reinterpret_cast<const char*>(path);
    }

    sqlite3_finalize(findStatement);

    if (storedPath.empty())
    {
        statusMessage = "Document record was not found.";
        return false;
    }

    fs::path filePath =
        gProjectRoot / fs::path(storedPath);

    std::error_code fileError;

    fs::remove(filePath, fileError);

    if (fileError)
    {
        statusMessage =
            "Could not remove the document file: " +
            fileError.message();

        return false;
    }

    sqlite3_stmt* tagDeleteStatement = nullptr;
    if (sqlite3_prepare_v2(gDatabase, "DELETE FROM document_tags WHERE document_id = ?;", -1, &tagDeleteStatement, nullptr) == SQLITE_OK)
    {
        sqlite3_bind_int(tagDeleteStatement, 1, id);
        sqlite3_step(tagDeleteStatement);
        sqlite3_finalize(tagDeleteStatement);
    }

    sqlite3_stmt* deleteStatement = nullptr;

    if (sqlite3_prepare_v2(
        gDatabase,
        "DELETE FROM documents WHERE id = ?;",
        -1,
        &deleteStatement,
        nullptr
    ) != SQLITE_OK)
    {
        statusMessage = sqlite3_errmsg(gDatabase);
        return false;
    }

    sqlite3_bind_int(deleteStatement, 1, id);

    bool success =
        sqlite3_step(deleteStatement) == SQLITE_DONE;

    sqlite3_finalize(deleteStatement);

    if (!success)
    {
        statusMessage = sqlite3_errmsg(gDatabase);
        return false;
    }

    LoadDocuments();

    statusMessage = "Document deleted successfully.";

    return true;
}

static void LoadFonts(ImGuiIO& io)
{
    const char* candidates[] = {
        "C:/Windows/Fonts/segoeui.ttf",
        "C:/Windows/Fonts/arial.ttf"
    };

    for (const char* path : candidates)
    {
        FILE* file = std::fopen(path, "rb");

        if (!file)
            continue;

        std::fclose(file);

        if (io.Fonts->AddFontFromFileTTF(path, 18.0f))
            return;
    }

    io.Fonts->AddFontDefault();
}

void ApplyLightTheme()
{
    ImGui::StyleColorsLight();

    ImGuiStyle& style = ImGui::GetStyle();

    style.WindowRounding = 10.0f;
    style.ChildRounding = 12.0f;
    style.FrameRounding = 7.0f;
    style.PopupRounding = 7.0f;
    style.ScrollbarRounding = 8.0f;
    style.GrabRounding = 7.0f;

    style.WindowPadding = ImVec2(22, 22);
    style.FramePadding = ImVec2(14, 10);
    style.ItemSpacing = ImVec2(16, 14);

    style.WindowBorderSize = 0.0f;
    style.ChildBorderSize = 1.0f;

    ImVec4* colors = style.Colors;

    colors[ImGuiCol_WindowBg] =
        ImVec4(0.96f, 0.97f, 0.99f, 1.00f);

    colors[ImGuiCol_ChildBg] =
        ImVec4(1.00f, 1.00f, 1.00f, 1.00f);

    colors[ImGuiCol_Text] =
        ImVec4(0.13f, 0.18f, 0.27f, 1.00f);

    colors[ImGuiCol_TextDisabled] =
        ImVec4(0.48f, 0.53f, 0.62f, 1.00f);

    colors[ImGuiCol_Border] =
        ImVec4(0.89f, 0.91f, 0.94f, 1.00f);

    colors[ImGuiCol_FrameBg] =
        ImVec4(0.95f, 0.96f, 0.98f, 1.00f);

    colors[ImGuiCol_FrameBgHovered] =
        ImVec4(0.90f, 0.94f, 0.99f, 1.00f);

    colors[ImGuiCol_FrameBgActive] =
        ImVec4(0.86f, 0.92f, 0.99f, 1.00f);

    colors[ImGuiCol_Button] =
        ImVec4(0.15f, 0.39f, 0.75f, 1.00f);

    colors[ImGuiCol_ButtonHovered] =
        ImVec4(0.11f, 0.33f, 0.66f, 1.00f);

    colors[ImGuiCol_ButtonActive] =
        ImVec4(0.08f, 0.27f, 0.56f, 1.00f);

    colors[ImGuiCol_Header] =
        ImVec4(0.89f, 0.93f, 0.99f, 1.00f);

    colors[ImGuiCol_HeaderHovered] =
        ImVec4(0.84f, 0.90f, 0.98f, 1.00f);

    colors[ImGuiCol_HeaderActive] =
        ImVec4(0.79f, 0.87f, 0.98f, 1.00f);

    colors[ImGuiCol_Separator] =
        ImVec4(0.88f, 0.90f, 0.93f, 1.00f);

    colors[ImGuiCol_CheckMark] =
        ImVec4(0.12f, 0.39f, 0.78f, 1.00f);
}

static void ApplyDarkTheme()
{
    ImGui::StyleColorsDark();

    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 10.0f;
    style.ChildRounding = 12.0f;
    style.FrameRounding = 7.0f;
    style.PopupRounding = 7.0f;
    style.ScrollbarRounding = 8.0f;
    style.GrabRounding = 7.0f;
    style.WindowPadding = ImVec2(22, 22);
    style.FramePadding = ImVec2(14, 10);
    style.ItemSpacing = ImVec2(16, 14);
    style.WindowBorderSize = 0.0f;
    style.ChildBorderSize = 1.0f;

    ImVec4* colors = style.Colors;
    colors[ImGuiCol_WindowBg] = ImVec4(0.075f, 0.085f, 0.11f, 1.0f);
    colors[ImGuiCol_ChildBg] = ImVec4(0.10f, 0.115f, 0.15f, 1.0f);
    colors[ImGuiCol_PopupBg] = ImVec4(0.11f, 0.125f, 0.16f, 0.98f);
    colors[ImGuiCol_Text] = ImVec4(0.91f, 0.93f, 0.96f, 1.0f);
    colors[ImGuiCol_TextDisabled] = ImVec4(0.55f, 0.59f, 0.66f, 1.0f);
    colors[ImGuiCol_Border] = ImVec4(0.22f, 0.25f, 0.31f, 1.0f);
    colors[ImGuiCol_FrameBg] = ImVec4(0.14f, 0.16f, 0.20f, 1.0f);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.19f, 0.23f, 0.30f, 1.0f);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.21f, 0.27f, 0.36f, 1.0f);
    colors[ImGuiCol_Button] = ImVec4(0.15f, 0.39f, 0.75f, 1.0f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.20f, 0.47f, 0.85f, 1.0f);
    colors[ImGuiCol_ButtonActive] = ImVec4(0.11f, 0.32f, 0.65f, 1.0f);
    colors[ImGuiCol_Header] = ImVec4(0.16f, 0.25f, 0.39f, 1.0f);
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.19f, 0.33f, 0.52f, 1.0f);
    colors[ImGuiCol_HeaderActive] = ImVec4(0.21f, 0.38f, 0.60f, 1.0f);
    colors[ImGuiCol_Separator] = ImVec4(0.22f, 0.25f, 0.31f, 1.0f);
    colors[ImGuiCol_CheckMark] = ImVec4(0.35f, 0.62f, 0.98f, 1.0f);
}

static void ApplyCurrentTheme()
{
    if (gDarkTheme)
        ApplyDarkTheme();
    else
        ApplyLightTheme();
}

static fs::path GetAppearanceSettingsPath()
{
    return gProjectRoot / "appearance.cfg";
}

static void LoadAppearancePreference()
{
    if (gProjectRoot.empty())
        return;

    std::ifstream input(GetAppearanceSettingsPath());
    std::string preference;
    if (input >> preference)
        gDarkTheme = (preference == "dark");
}

static void SaveAppearancePreference()
{
    if (gProjectRoot.empty())
        return;

    std::ofstream output(GetAppearanceSettingsPath(), std::ios::trunc);
    if (output)
        output << (gDarkTheme ? "dark" : "light") << "\\n";
}

void DrawSidebar()
{
    ImGui::BeginChild(
        "Sidebar",
        ImVec2(kSidebarWidth, 0),
        true
    );

    ImGui::TextColored(
        ImVec4(0.12f, 0.36f, 0.72f, 1.0f),
        "DOCUMIND"
    );

    ImGui::SameLine();
    ImGui::TextDisabled("AI");

    ImGui::TextDisabled("DOCUMENT INTELLIGENCE");

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    const char* menuItems[] = {
        "Dashboard",
        "My Documents",
        "AI Assistant",
        "Key Information",
        "Document Comparison",
        "Summaries",
        "Export Center",
        "Appearance",
        "Duplicate Detector"
    };

    ImGui::PushStyleVar(
        ImGuiStyleVar_SelectableTextAlign,
        ImVec2(0.0f, 0.5f)
    );

    for (int i = 0; i < 9; i++)
    {
        std::string label =
            std::string("   ") +
            menuItems[i] +
            "##menu" +
            std::to_string(i);

        if (ImGui::Selectable(
            label.c_str(),
            selectedPage == i,
            0,
            ImVec2(0, 40)))
        {
            selectedPage = i;

            statusMessage =
                std::string(menuItems[i]) + " selected";
        }
    }

    ImGui::PopStyleVar();

    float footerY =
        ImGui::GetWindowHeight() - 110.0f;

    footerY = std::max(
        footerY,
        ImGui::GetCursorPosY() + 10.0f
    );

    ImGui::SetCursorPosY(footerY);

    ImGui::Separator();
    ImGui::Spacing();

    ImGui::TextDisabled("DocuMind AI");
    ImGui::TextDisabled("Version 1.0 - Prototype");

    ImGui::EndChild();
}

void DrawStatCard(
    const char* label,
    const char* number,
    const char* description,
    float width)
{
    // Keep statistic cards compact and prevent unnecessary inner scrollbars.
    // The extra height allows all four text elements to fit with the app's padding.
    ImGui::BeginChild(
        label,
        ImVec2(width, 140),
        true,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse
    );

    ImGui::TextDisabled("%s", label);
    ImGui::Spacing();
    ImGui::Text("%s", number);
    ImGui::TextDisabled("%s", description);

    ImGui::EndChild();
}

static float GetMainContentWidth()
{
    float width =
        ImGui::GetContentRegionAvail().x
        - kAssistantWidth
        - ImGui::GetStyle().ItemSpacing.x;

    return std::max(width, 300.0f);
}

void DrawUploadButton()
{
    if (ImGui::Button(
        "+ New Document",
        ImVec2(150, 38)))
    {
        OpenDocumentPicker();
    }
}

struct DashboardTagStat
{
    std::string tag;
    int documentCount = 0;
};

static std::vector<DashboardTagStat> GetDashboardTopTags(int limit = 6)
{
    std::vector<DashboardTagStat> result;
    if (!gDatabase)
        return result;

    sqlite3_stmt* statement = nullptr;
    const char* sql =
        "SELECT tag, COUNT(DISTINCT document_id) AS document_count "
        "FROM document_tags GROUP BY tag COLLATE NOCASE "
        "ORDER BY document_count DESC, tag COLLATE NOCASE LIMIT ?;";

    if (sqlite3_prepare_v2(gDatabase, sql, -1, &statement, nullptr) != SQLITE_OK)
        return result;

    sqlite3_bind_int(statement, 1, limit);
    while (sqlite3_step(statement) == SQLITE_ROW)
    {
        const unsigned char* tag = sqlite3_column_text(statement, 0);
        DashboardTagStat item;
        item.tag = tag ? reinterpret_cast<const char*>(tag) : "";
        item.documentCount = sqlite3_column_int(statement, 1);
        if (!item.tag.empty())
            result.push_back(std::move(item));
    }
    sqlite3_finalize(statement);
    return result;
}

static bool DashboardDocumentHasImportantTag(const Document& document)
{
    for (const std::string& tag : document.tags)
    {
        std::string normalized = tag;
        std::transform(normalized.begin(), normalized.end(), normalized.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (normalized == "important")
            return true;
    }
    return false;
}

static void DashboardOpenDocument(const Document& document)
{
    selectedPage = 1;
    gCategoryFilterId = 0;
    std::snprintf(documentSearch, sizeof(documentSearch), "%s", document.name.c_str());
    statusMessage = "Showing selected document: " + document.name;
}

static void DrawDashboardPanelTitle(const char* title, const char* subtitle)
{
    ImGui::Text("%s", title);
    if (subtitle && subtitle[0] != '\0')
        ImGui::TextDisabled("%s", subtitle);
    ImGui::Spacing();
}

void DrawDashboard()
{
    ImGui::BeginChild("MainContent", ImVec2(GetMainContentWidth(), 0), true);

    ImGui::Text("Your document workspace");
    ImGui::TextDisabled("A live overview of your documents, categories, tags and AI work.");

    const float buttonWidth = 150.0f;
    ImGui::SameLine(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - buttonWidth);
    DrawUploadButton();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    const float gap = ImGui::GetStyle().ItemSpacing.x;
    const float availableWidth = ImGui::GetContentRegionAvail().x;
    const float cardWidth = std::max(150.0f, (availableWidth - gap * 2.0f) / 3.0f);

    const std::string documentCount = std::to_string(gDocuments.size());
    const std::string storage = FormatFileSize(GetTotalStorage());
    const std::string summaryCount = std::to_string(gSummaries.size());

    DrawStatCard("TOTAL DOCUMENTS", documentCount.c_str(), "Documents in your library", cardWidth);
    ImGui::SameLine();
    DrawStatCard("SAVED SUMMARIES", summaryCount.c_str(), "AI summaries saved", cardWidth);
    ImGui::SameLine();
    DrawStatCard("STORAGE USED", storage.c_str(), "Local document storage", cardWidth);

    ImGui::Spacing();
    ImGui::Spacing();

    // Category distribution is calculated from the live in-memory document list.
    ImGui::BeginChild("DashboardCategoryPanel", ImVec2(0, 215), true);
    DrawDashboardPanelTitle("Documents by category", "Your library grouped by its current categories");

    if (gDocuments.empty())
    {
        ImGui::TextDisabled("Upload documents to see category statistics.");
    }
    else if (gCategories.empty())
    {
        ImGui::TextDisabled("No categories are available yet.");
    }
    else
    {
        const float maxBarWidth = std::max(100.0f, ImGui::GetContentRegionAvail().x - 190.0f);
        int maxCount = 1;
        std::vector<int> categoryCounts(gCategories.size(), 0);
        for (const Document& document : gDocuments)
        {
            for (size_t i = 0; i < gCategories.size(); ++i)
            {
                if (document.categoryId == gCategories[i].id)
                {
                    ++categoryCounts[i];
                    maxCount = std::max(maxCount, categoryCounts[i]);
                    break;
                }
            }
        }

        for (size_t i = 0; i < gCategories.size(); ++i)
        {
            if (categoryCounts[i] == 0)
                continue;

            ImGui::PushID(static_cast<int>(gCategories[i].id));
            ImGui::Text("%s", gCategories[i].name.c_str());
            ImGui::SameLine(155.0f);
            const float barWidth = maxBarWidth * static_cast<float>(categoryCounts[i]) / static_cast<float>(maxCount);
            const ImVec2 barStart = ImGui::GetCursorScreenPos();
            const float barHeight = 13.0f;
            ImDrawList* drawList = ImGui::GetWindowDrawList();
            drawList->AddRectFilled(barStart, ImVec2(barStart.x + maxBarWidth, barStart.y + barHeight), IM_COL32(232, 237, 245, 255), 4.0f);
            drawList->AddRectFilled(barStart, ImVec2(barStart.x + barWidth, barStart.y + barHeight), IM_COL32(48, 112, 190, 255), 4.0f);
            ImGui::Dummy(ImVec2(maxBarWidth, barHeight));
            ImGui::SameLine();
            ImGui::Text("%d", categoryCounts[i]);
            ImGui::PopID();
        }
    }
    ImGui::EndChild();

    ImGui::Spacing();

    // Recent documents and important documents are quick links into the library.
    const float panelGap = ImGui::GetStyle().ItemSpacing.x;
    const float panelWidth = (ImGui::GetContentRegionAvail().x - panelGap) * 0.5f;
    ImGui::BeginChild("DashboardRecentPanel", ImVec2(panelWidth, 245), true);
    DrawDashboardPanelTitle("Recently uploaded", "Latest documents added to your library");

    if (gDocuments.empty())
    {
        ImGui::TextDisabled("No documents yet.");
        if (ImGui::Button("Upload your first document", ImVec2(-1, 34)))
            OpenDocumentPicker();
    }
    else
    {
        const int recentCount = std::min(static_cast<int>(gDocuments.size()), 5);
        for (int i = 0; i < recentCount; ++i)
        {
            const Document& document = gDocuments[static_cast<size_t>(i)];
            ImGui::PushID(document.id);
            if (ImGui::Selectable(document.name.c_str(), false, 0, ImVec2(0, 25)))
                DashboardOpenDocument(document);
            ImGui::TextDisabled("%s  |  %s", document.categoryName.c_str(), FormatFileSize(document.size).c_str());
            ImGui::PopID();
            if (i + 1 < recentCount)
                ImGui::Separator();
        }
    }
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("DashboardImportantPanel", ImVec2(0, 245), true);
    DrawDashboardPanelTitle("Important documents", "Documents tagged Important");

    int importantCount = 0;
    for (const Document& document : gDocuments)
    {
        if (!DashboardDocumentHasImportantTag(document))
            continue;
        ++importantCount;
        ImGui::PushID(document.id);
        if (ImGui::Selectable(document.name.c_str(), false, 0, ImVec2(0, 25)))
            DashboardOpenDocument(document);
        ImGui::TextDisabled("%s  |  %s", document.categoryName.c_str(), FormatFileSize(document.size).c_str());
        ImGui::PopID();
    }
    if (importantCount == 0)
        ImGui::TextDisabled("No documents have the Important tag yet. Add this tag in My Documents for quick access here.");
    ImGui::EndChild();

    ImGui::Spacing();

    ImGui::BeginChild("DashboardTagsPanel", ImVec2(0, 190), true);
    DrawDashboardPanelTitle("Most-used tags", "Ranked by the number of documents carrying each tag");
    const std::vector<DashboardTagStat> topTags = GetDashboardTopTags(6);
    if (topTags.empty())
    {
        ImGui::TextDisabled("No tags have been added yet.");
    }
    else
    {
        int maxTagCount = 1;
        for (const DashboardTagStat& item : topTags)
            maxTagCount = std::max(maxTagCount, item.documentCount);
        const float maxTagBarWidth = std::max(100.0f, ImGui::GetContentRegionAvail().x - 190.0f);
        for (size_t i = 0; i < topTags.size(); ++i)
        {
            ImGui::PushID(static_cast<int>(i));
            ImGui::Text("%s", topTags[i].tag.c_str());
            ImGui::SameLine(155.0f);
            const float barWidth = maxTagBarWidth * static_cast<float>(topTags[i].documentCount) / static_cast<float>(maxTagCount);
            const ImVec2 barStart = ImGui::GetCursorScreenPos();
            const float barHeight = 13.0f;
            ImDrawList* drawList = ImGui::GetWindowDrawList();
            drawList->AddRectFilled(barStart, ImVec2(barStart.x + maxTagBarWidth, barStart.y + barHeight), IM_COL32(232, 237, 245, 255), 4.0f);
            drawList->AddRectFilled(barStart, ImVec2(barStart.x + barWidth, barStart.y + barHeight), IM_COL32(40, 150, 120, 255), 4.0f);
            ImGui::Dummy(ImVec2(maxTagBarWidth, barHeight));
            ImGui::SameLine();
            ImGui::Text("%d", topTags[i].documentCount);
            ImGui::PopID();
        }
    }
    ImGui::EndChild();

    ImGui::Spacing();

    ImGui::BeginChild("DashboardFileTypesPanel", ImVec2(0, 185), true);
    DrawDashboardPanelTitle("Documents by file type", "A quick breakdown of PDF, Word and text files");

    if (gDocuments.empty())
    {
        ImGui::TextDisabled("Upload documents to see file type statistics.");
    }
    else
    {
        std::vector<std::pair<std::string, int>> typeCounts;
        for (const Document& document : gDocuments)
        {
            std::string type = document.type.empty() ? "Unknown" : document.type;
            auto found = std::find_if(typeCounts.begin(), typeCounts.end(),
                [&type](const auto& item) { return item.first == type; });
            if (found == typeCounts.end())
                typeCounts.push_back({type, 1});
            else
                ++found->second;
        }

        std::sort(typeCounts.begin(), typeCounts.end(),
            [](const auto& a, const auto& b) { return a.second > b.second; });

        const float maxBarWidth = std::max(100.0f, ImGui::GetContentRegionAvail().x - 190.0f);
        const int maxCount = typeCounts.empty() ? 1 : typeCounts.front().second;
        for (size_t i = 0; i < typeCounts.size(); ++i)
        {
            ImGui::PushID(static_cast<int>(i) + 7000);
            ImGui::Text("%s", typeCounts[i].first.c_str());
            ImGui::SameLine(155.0f);
            const float barWidth = maxBarWidth * static_cast<float>(typeCounts[i].second) /
                static_cast<float>(std::max(1, maxCount));
            const ImVec2 barStart = ImGui::GetCursorScreenPos();
            const float barHeight = 13.0f;
            ImDrawList* drawList = ImGui::GetWindowDrawList();
            drawList->AddRectFilled(barStart, ImVec2(barStart.x + maxBarWidth, barStart.y + barHeight), IM_COL32(232, 237, 245, 255), 4.0f);
            drawList->AddRectFilled(barStart, ImVec2(barStart.x + barWidth, barStart.y + barHeight), IM_COL32(125, 91, 190, 255), 4.0f);
            ImGui::Dummy(ImVec2(maxBarWidth, barHeight));
            ImGui::SameLine();
            ImGui::Text("%d", typeCounts[i].second);
            ImGui::PopID();
        }
    }
    ImGui::EndChild();

    ImGui::Spacing();
    ImGui::TextDisabled("%s", statusMessage.c_str());
    ImGui::EndChild();
}

void DrawDocumentsPage()
{
    static bool openDeletePopup = false;
    static bool openDetailsPopup = false;
    static bool openCategoryManager = false;

    ImGui::BeginChild("DocumentsPage", ImVec2(GetMainContentWidth(), 0), true);
    ImGui::Text("My Documents");
    ImGui::TextDisabled("Browse, search, categorize and tag your document library.");
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (ImGui::Button("+ Upload Document", ImVec2(180, 40))) OpenDocumentPicker();
    ImGui::SameLine();
    if (ImGui::Button("Manage Categories", ImVec2(170, 40))) openCategoryManager = true;

    ImGui::Spacing();
    ImGui::Text("Total documents: %d", static_cast<int>(gDocuments.size()));
    ImGui::SameLine();
    ImGui::TextDisabled("| Storage: %s", FormatFileSize(GetTotalStorage()).c_str());
    ImGui::Spacing();

    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##DocumentSearch", "Search by filename or tag...", documentSearch, sizeof(documentSearch));

    ImGui::TextDisabled("Filter by category");
    const char* selectedFilterName = "All categories";
    for (const DocumentCategory& category : gCategories)
        if (category.id == gCategoryFilterId) selectedFilterName = category.name.c_str();
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##CategoryFilter", selectedFilterName))
    {
        if (ImGui::Selectable("All categories", gCategoryFilterId == 0)) gCategoryFilterId = 0;
        for (const DocumentCategory& category : gCategories)
            if (ImGui::Selectable(category.name.c_str(), gCategoryFilterId == category.id)) gCategoryFilterId = category.id;
        ImGui::EndCombo();
    }

    ImGui::Spacing();
    if (ImGui::BeginTable("DocumentTable", 5,
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY,
        ImVec2(0, 350)))
    {
        ImGui::TableSetupColumn("Document / Organization", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 55);
        ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 75);
        ImGui::TableSetupColumn("Date Added", ImGuiTableColumnFlags_WidthFixed, 110);
        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, 290);
        ImGui::TableHeadersRow();

        int visibleCount = 0;
        std::string searchTerm = documentSearch;
        std::transform(searchTerm.begin(), searchTerm.end(), searchTerm.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        for (const Document& document : gDocuments)
        {
            if (gCategoryFilterId != 0 && document.categoryId != gCategoryFilterId) continue;

            std::string searchable = document.name;
            searchable += " ";
            searchable += document.categoryName;
            for (const std::string& tag : document.tags) { searchable += " "; searchable += tag; }
            std::transform(searchable.begin(), searchable.end(), searchable.begin(),
                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (!searchTerm.empty() && searchable.find(searchTerm) == std::string::npos) continue;

            visibleCount++;
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextWrapped("%s", document.name.c_str());
            ImGui::TextColored(ImVec4(0.15f, 0.39f, 0.75f, 1.0f), "%s", document.categoryName.c_str());
            if (!document.tags.empty())
            {
                std::string tagLine = "Tags: ";
                for (size_t i = 0; i < document.tags.size(); ++i)
                {
                    if (i) tagLine += ", ";
                    tagLine += document.tags[i];
                }
                ImGui::TextDisabled("%s", tagLine.c_str());
            }
            else ImGui::TextDisabled("No tags");

            ImGui::TableSetColumnIndex(1); ImGui::Text("%s", document.type.c_str());
            ImGui::TableSetColumnIndex(2); ImGui::Text("%s", FormatFileSize(document.size).c_str());
            ImGui::TableSetColumnIndex(3); ImGui::Text("%s", document.addedAt.c_str());
            ImGui::TableSetColumnIndex(4);

            const std::string suffix = "##" + std::to_string(document.id);
            if (ImGui::SmallButton(("Open" + suffix).c_str()))
            {
#ifdef _WIN32
                fs::path fullPath = gProjectRoot / fs::path(document.path);
                if (fs::exists(fullPath))
                {
                    HINSTANCE result = ShellExecuteA(nullptr, "open", fullPath.string().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                    statusMessage = reinterpret_cast<INT_PTR>(result) > 32 ? "Opening: " + document.name : "Windows could not open this document.";
                }
                else statusMessage = "Document file could not be found.";
#else
                statusMessage = "Opening documents is supported on Windows.";
#endif
            }
            ImGui::SameLine();
#ifdef _WIN32
            if (ImGui::SmallButton(("Extract" + suffix).c_str())) ExtractDocumentText(document);
            ImGui::SameLine();
#endif
            if (ImGui::SmallButton(("Organize" + suffix).c_str()))
            {
                gOrganizationDocumentId = document.id;
                gEditDocumentCategoryId = document.categoryId;
                gDocumentTagInput[0] = '\0';
                openDetailsPopup = true;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton(("Delete" + suffix).c_str()))
            {
                pendingDeleteId = document.id;
                openDeletePopup = true;
            }
        }

        if (visibleCount == 0)
        {
            ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0);
            ImGui::TextDisabled(gDocuments.empty() ? "No documents uploaded yet." : "No documents match this search or category filter.");
        }
        ImGui::EndTable();
    }

    // Document organization editor (category and multiple tags).
    if (openDetailsPopup) { ImGui::OpenPopup("Organize Document"); openDetailsPopup = false; }
    ImGui::SetNextWindowSize(ImVec2(520, 430), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Organize Document", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        auto documentIt = std::find_if(gDocuments.begin(), gDocuments.end(),
            [](const Document& item) { return item.id == gOrganizationDocumentId; });
        if (documentIt != gDocuments.end())
        {
            ImGui::TextWrapped("%s", documentIt->name.c_str());
            ImGui::Separator();
            const char* currentCategory = "Uncategorized";
            for (const DocumentCategory& category : gCategories)
                if (category.id == gEditDocumentCategoryId) currentCategory = category.name.c_str();
            ImGui::Text("Category");
            ImGui::SetNextItemWidth(-1);
            if (ImGui::BeginCombo("##EditDocumentCategory", currentCategory))
            {
                for (const DocumentCategory& category : gCategories)
                    if (ImGui::Selectable(category.name.c_str(), gEditDocumentCategoryId == category.id)) gEditDocumentCategoryId = category.id;
                ImGui::EndCombo();
            }
            if (ImGui::Button("Save Category", ImVec2(140, 32))) UpdateDocumentCategory(gOrganizationDocumentId, gEditDocumentCategoryId);
            ImGui::Spacing(); ImGui::Separator(); ImGui::Text("Tags");
            ImGui::SetNextItemWidth(300);
            ImGui::InputTextWithHint("##NewDocumentTag", "Enter a tag...", gDocumentTagInput, sizeof(gDocumentTagInput));
            ImGui::SameLine();
            if (ImGui::Button("Add Tag"))
            {
                if (AddDocumentTag(gOrganizationDocumentId, gDocumentTagInput)) gDocumentTagInput[0] = '\0';
            }
            // Reloaded document list is safe to inspect after each tag operation.
            documentIt = std::find_if(gDocuments.begin(), gDocuments.end(),
                [](const Document& item) { return item.id == gOrganizationDocumentId; });
            if (documentIt != gDocuments.end())
            {
                for (size_t i = 0; i < documentIt->tags.size(); ++i)
                {
                    ImGui::BulletText("%s", documentIt->tags[i].c_str());
                    ImGui::SameLine();
                    if (ImGui::SmallButton(("Remove##tag" + std::to_string(i)).c_str()))
                    {
                        const std::string tag = documentIt->tags[i];
                        RemoveDocumentTag(gOrganizationDocumentId, tag);
                        break;
                    }
                }
                if (documentIt->tags.empty()) ImGui::TextDisabled("No tags have been added.");
            }
        }
        else ImGui::TextDisabled("This document is no longer available.");
        ImGui::Spacing();
        if (ImGui::Button("Close", ImVec2(110, 34))) { gOrganizationDocumentId = -1; ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }

    // Category manager: add, rename, or remove without deleting documents.
    if (openCategoryManager) { ImGui::OpenPopup("Manage Document Categories"); openCategoryManager = false; }
    ImGui::SetNextWindowSize(ImVec2(500, 430), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Manage Document Categories", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text("Create a category");
        ImGui::SetNextItemWidth(300);
        ImGui::InputTextWithHint("##NewCategory", "Category name", gNewCategoryInput, sizeof(gNewCategoryInput));
        ImGui::SameLine();
        if (ImGui::Button("Add Category"))
        {
            if (AddCategory(gNewCategoryInput)) { gNewCategoryInput[0] = '\0'; gSelectedCategoryToEdit = -1; }
        }
        ImGui::Separator(); ImGui::Text("Rename or remove a category");
        const char* selectedName = "Select category";
        for (const DocumentCategory& category : gCategories)
            if (category.id == gSelectedCategoryToEdit) selectedName = category.name.c_str();
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##CategoryToEdit", selectedName))
        {
            for (const DocumentCategory& category : gCategories)
            {
                if (ImGui::Selectable(category.name.c_str(), gSelectedCategoryToEdit == category.id))
                {
                    gSelectedCategoryToEdit = category.id;
                    std::snprintf(gRenameCategoryInput, sizeof(gRenameCategoryInput), "%s", category.name.c_str());
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SetNextItemWidth(300);
        ImGui::InputText("##RenameCategory", gRenameCategoryInput, sizeof(gRenameCategoryInput));
        ImGui::BeginDisabled(gSelectedCategoryToEdit == -1 || gSelectedCategoryToEdit == GetUncategorizedCategoryId());
        if (ImGui::Button("Rename Category", ImVec2(150, 34)))
        {
            if (gSelectedCategoryToEdit != -1) RenameCategory(gSelectedCategoryToEdit, gRenameCategoryInput);
            else statusMessage = "Select a category first.";
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(gSelectedCategoryToEdit == -1 || gSelectedCategoryToEdit == GetUncategorizedCategoryId());
        if (ImGui::Button("Remove Category", ImVec2(150, 34)))
        {
            if (gSelectedCategoryToEdit != -1 && DeleteCategory(gSelectedCategoryToEdit))
            {
                gSelectedCategoryToEdit = -1;
                gRenameCategoryInput[0] = '\0';
            }
        }
        ImGui::EndDisabled();
        ImGui::TextWrapped("Removing a category moves its documents to Uncategorized. It never deletes document files.");
        ImGui::Spacing();
        if (ImGui::Button("Close", ImVec2(110, 34))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // Existing extracted-document reader.
    if (gOpenReaderPopup) { ImGui::OpenPopup("Extracted Document Text"); gOpenReaderPopup = false; }
    ImGui::SetNextWindowSize(ImVec2(800, 600), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Extracted Document Text", nullptr, ImGuiWindowFlags_NoSavedSettings))
    {
        ImGui::Text("Document: %s", gExtractedDocumentName.c_str());
        ImGui::Separator(); ImGui::Spacing();
        ImGui::BeginChild("ExtractedTextContent", ImVec2(0, 430), true);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
        ImGui::TextUnformatted(gExtractedDocumentText.c_str());
        ImGui::PopTextWrapPos(); ImGui::EndChild(); ImGui::Spacing();
#ifdef _WIN32
        if (ImGui::Button("Generate Short Summary", ImVec2(190, 38))) SummarizeDocumentText(gExtractedDocumentName, gExtractedDocumentText, "short");
        ImGui::SameLine();
        if (ImGui::Button("Generate Detailed Summary", ImVec2(210, 38))) SummarizeDocumentText(gExtractedDocumentName, gExtractedDocumentText, "detailed");
        ImGui::SameLine();
#endif
        if (ImGui::Button("Close", ImVec2(100, 38))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // Existing deletion confirmation.
    if (openDeletePopup) { ImGui::OpenPopup("Confirm Document Deletion"); openDeletePopup = false; }
    if (ImGui::BeginPopupModal("Confirm Document Deletion", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text("Are you sure you want to delete this document?"); ImGui::Spacing();
        if (ImGui::Button("Yes, Delete", ImVec2(120, 38)))
        {
            if (pendingDeleteId != -1) DeleteDocument(pendingDeleteId);
            pendingDeleteId = -1; ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(100, 38))) { pendingDeleteId = -1; ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }

    ImGui::Spacing();
    ImGui::TextDisabled("%s", statusMessage.c_str());
    ImGui::EndChild();
}

void DrawPlaceholderPage(
    const char* id,
    const char* title,
    const char* description)
{
    ImGui::BeginChild(
        id,
        ImVec2(GetMainContentWidth(), 0),
        true
    );

    ImGui::Text("%s", title);

    ImGui::TextDisabled(
        "%s",
        description
    );

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::BeginChild(
        "PlaceholderCard",
        ImVec2(0, 120),
        true
    );

    ImGui::TextDisabled("Coming soon");

    ImGui::Text(
        "This page will be built in a later development stage."
    );

    ImGui::EndChild();

    ImGui::Spacing();

    ImGui::TextDisabled(
        "%s",
        statusMessage.c_str()
    );

    ImGui::EndChild();
}

void DrawSummariesPage()
{
    static int pendingSummaryDeleteId = -1;
    static bool openSummaryDeletePopup = false;

    ImGui::BeginChild(
        "SummariesPage",
        ImVec2(GetMainContentWidth(), 0),
        true
    );

    ImGui::Text("Summary History");
    ImGui::TextDisabled(
        "Your summaries are saved permanently in the local SQLite database."
    );
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::Text("Total saved summaries: %d",
        static_cast<int>(gSummaries.size()));

    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint(
        "##SummarySearch",
        "Search summaries by document filename...",
        summarySearch,
        sizeof(summarySearch)
    );

    ImGui::Spacing();

    std::string searchTerm = summarySearch;
    std::transform(
        searchTerm.begin(),
        searchTerm.end(),
        searchTerm.begin(),
        [](unsigned char c)
        {
            return static_cast<char>(std::tolower(c));
        }
    );

    int visibleCount = 0;

    ImGui::BeginChild(
        "SummaryHistoryList",
        ImVec2(0, 270),
        true
    );

    for (const DocumentSummary& item : gSummaries)
    {
        std::string filenameLower = item.filename;
        std::transform(
            filenameLower.begin(),
            filenameLower.end(),
            filenameLower.begin(),
            [](unsigned char c)
            {
                return static_cast<char>(std::tolower(c));
            }
        );

        if (!searchTerm.empty() &&
            filenameLower.find(searchTerm) == std::string::npos)
        {
            continue;
        }

        visibleCount++;
        ImGui::PushID(item.id);

        ImGui::TextWrapped("%s", item.filename.c_str());
        ImGui::TextDisabled(
            "%s summary  |  %s",
            item.summaryType.c_str(),
            item.generatedAt.c_str()
        );

        if (ImGui::SmallButton("View"))
            gSelectedSummaryId = item.id;

        ImGui::SameLine();

        if (ImGui::SmallButton("Delete"))
        {
            pendingSummaryDeleteId = item.id;
            openSummaryDeletePopup = true;
        }

        ImGui::Separator();
        ImGui::PopID();
    }

    if (visibleCount == 0)
    {
        ImGui::TextDisabled(
            gSummaries.empty()
                ? "No summaries saved yet. Generate a summary from an extracted document."
                : "No summaries match your search."
        );
    }

    ImGui::EndChild();

    const DocumentSummary* selected = nullptr;

    for (const DocumentSummary& item : gSummaries)
    {
        if (item.id == gSelectedSummaryId)
        {
            selected = &item;
            break;
        }
    }

    if (selected)
    {
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::TextWrapped("%s", selected->filename.c_str());
        ImGui::TextDisabled(
            "%s summary generated %s",
            selected->summaryType.c_str(),
            selected->generatedAt.c_str()
        );

        ImGui::BeginChild(
            "SummaryText",
            ImVec2(0, 220),
            true
        );

        ImGui::PushTextWrapPos(
            ImGui::GetCursorPosX() +
            ImGui::GetContentRegionAvail().x
        );
        ImGui::TextUnformatted(selected->text.c_str());
        ImGui::PopTextWrapPos();

        ImGui::EndChild();
    }

    if (openSummaryDeletePopup)
    {
        ImGui::OpenPopup("Confirm Summary Deletion");
        openSummaryDeletePopup = false;
    }

    if (ImGui::BeginPopupModal(
        "Confirm Summary Deletion",
        nullptr,
        ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text("Are you sure you want to delete this saved summary?");
        ImGui::Spacing();

        if (ImGui::Button("Yes, Delete", ImVec2(120, 36)))
        {
            if (pendingSummaryDeleteId != -1)
                DeleteSummaryFromDatabase(pendingSummaryDeleteId);

            pendingSummaryDeleteId = -1;
            ImGui::CloseCurrentPopup();
        }

        ImGui::SameLine();

        if (ImGui::Button("Cancel", ImVec2(100, 36)))
        {
            pendingSummaryDeleteId = -1;
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }

    ImGui::Spacing();
    ImGui::TextWrapped("%s", statusMessage.c_str());
    ImGui::EndChild();
}

void DrawAIAssistantPage()
{
    ImGui::BeginChild(
        "AIChatWorkspace",
        ImVec2(GetMainContentWidth(), 0),
        true,
        ImGuiWindowFlags_AlwaysHorizontalScrollbar |
        ImGuiWindowFlags_AlwaysVerticalScrollbar
    );

    ImGui::Text("Chat with Your Documents");
    ImGui::TextDisabled(
        "Ask questions, save conversations and continue previous discussions."
    );
    ImGui::Separator();
    ImGui::Spacing();

    if (gDocuments.empty())
    {
        ImGui::TextWrapped(
            "Your document library is empty. Upload a PDF, DOCX or TXT file "
            "from My Documents before starting a chat."
        );
        ImGui::EndChild();
        return;
    }

    if (gSelectedChatDocumentId == -1 ||
        std::none_of(
            gDocuments.begin(),
            gDocuments.end(),
            [](const Document& item)
            {
                return item.id == gSelectedChatDocumentId;
            }))
    {
        gSelectedChatDocumentId = gDocuments.front().id;
    }

    const Document* selectedDocument = nullptr;
    for (const Document& item : gDocuments)
    {
        if (item.id == gSelectedChatDocumentId)
        {
            selectedDocument = &item;
            break;
        }
    }

    const char* preview = selectedDocument
        ? selectedDocument->name.c_str()
        : "Select a document";

    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##ChatDocument", preview))
    {
        for (const Document& item : gDocuments)
        {
            bool isSelected = item.id == gSelectedChatDocumentId;
            if (ImGui::Selectable(item.name.c_str(), isSelected))
            {
                if (gSelectedChatDocumentId != item.id)
                {
                    gSelectedChatDocumentId = item.id;
                    gExtractedDocumentText.clear();
                    gExtractedDocumentName.clear();
                    gChatMessages.clear();
                    gCurrentChatConversationId = -1;
                    gSelectedSavedConversationId = -1;
                    gChatQuestion[0] = '\0';
                }
            }
            if (isSelected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    bool documentLoaded =
        selectedDocument &&
        !gExtractedDocumentText.empty() &&
        gExtractedDocumentName == selectedDocument->name;

    if (ImGui::Button(
        documentLoaded ? "Reload Selected Document" : "Load Selected Document",
        ImVec2(220, 38)))
    {
        if (selectedDocument)
        {
            gChatMessages.clear();
            gCurrentChatConversationId = -1;
            gSelectedSavedConversationId = -1;
            gChatQuestion[0] = '\0';

            if (ExtractDocumentText(*selectedDocument))
            {
                gOpenReaderPopup = false;
                statusMessage =
                    "Document loaded. You can now ask questions about it.";
            }
        }
    }

    ImGui::SameLine();
    if (documentLoaded)
        ImGui::TextDisabled("Loaded: %s", gExtractedDocumentName.c_str());
    else
        ImGui::TextDisabled("Load a document to begin chatting.");

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Text("Saved Conversations");

    const ChatConversation* selectedSavedConversation = nullptr;
    for (const ChatConversation& item : gChatConversations)
    {
        if (item.id == gSelectedSavedConversationId)
        {
            selectedSavedConversation = &item;
            break;
        }
    }

    std::string savedPreview = selectedSavedConversation
        ? selectedSavedConversation->title
        : "Choose a saved conversation";

    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##SavedConversations", savedPreview.c_str()))
    {
        for (const ChatConversation& item : gChatConversations)
        {
            std::string label = item.title + "  |  " + item.updatedAt;
            bool isSelected = item.id == gSelectedSavedConversationId;
            if (ImGui::Selectable(label.c_str(), isSelected))
                gSelectedSavedConversationId = item.id;
            if (isSelected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    if (gSelectedSavedConversationId != -1)
    {
        if (ImGui::Button("Open Saved Conversation", ImVec2(210, 34)))
            LoadChatConversation(gSelectedSavedConversationId);

        ImGui::SameLine();
        if (ImGui::Button("Delete Saved Conversation", ImVec2(210, 34)))
            ImGui::OpenPopup("DeleteSavedChatPopup");
    }

    if (ImGui::BeginPopupModal(
        "DeleteSavedChatPopup", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextWrapped("Delete this saved conversation and all its messages? This cannot be undone.");
        ImGui::Spacing();
        if (ImGui::Button("Yes, Delete", ImVec2(120, 36)))
        {
            if (gSelectedSavedConversationId != -1)
                DeleteChatConversation(gSelectedSavedConversationId);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(100, 36)))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    ImGui::Spacing();
    ImGui::Separator();

    ImGui::BeginChild(
        "ChatConversation",
        ImVec2(0, 285),
        true
    );

    if (gChatMessages.empty())
    {
        ImGui::TextDisabled(
            "Your conversation will appear here. Try asking:\n"
            "\"What is this document about?\"\n"
            "\"What are the main points?\"\n"
            "\"Are there any important dates or deadlines?\""
        );
    }
    else
    {
        for (const ChatEntry& entry : gChatMessages)
        {
            ImGui::TextColored(
                entry.role == "user"
                    ? ImVec4(0.12f, 0.36f, 0.72f, 1.0f)
                    : ImVec4(0.10f, 0.48f, 0.30f, 1.0f),
                "%s",
                entry.role == "user" ? "You" : "DocuMind AI"
            );
            ImGui::PushTextWrapPos(
                ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x
            );
            ImGui::TextUnformatted(entry.content.c_str());
            ImGui::PopTextWrapPos();
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
        }
    }
    ImGui::EndChild();

    ImGui::Spacing();
    ImGui::Text("Your question");
    ImGui::InputTextMultiline(
        "##DocumentChatQuestion",
        gChatQuestion,
        sizeof(gChatQuestion),
        ImVec2(-1, 85)
    );

    if (!documentLoaded)
        ImGui::BeginDisabled();

    if (ImGui::Button("Send Question", ImVec2(160, 40)))
    {
        std::string question = gChatQuestion;
        if (AskDocumentQuestion(
            gExtractedDocumentName,
            gExtractedDocumentText,
            question))
        {
            gChatQuestion[0] = '\0';
        }
    }

    if (!documentLoaded)
        ImGui::EndDisabled();

    ImGui::SameLine();
    if (ImGui::Button("New Conversation", ImVec2(170, 40)))
    {
        gChatMessages.clear();
        gCurrentChatConversationId = -1;
        gSelectedSavedConversationId = -1;
        gChatQuestion[0] = '\0';
        statusMessage = "A new conversation is ready. Your previous chats remain saved.";
    }

    ImGui::Spacing();
    ImGui::TextWrapped("%s", statusMessage.c_str());
    ImGui::EndChild();
}


void DrawKeyInformationPage()
{
    ImGui::BeginChild(
        "KeyInformationWorkspace",
        ImVec2(GetMainContentWidth(), 0),
        true
    );

    ImGui::Text("AI Key Information Extraction");
    ImGui::TextDisabled(
        "Find names, dates, contact details, financial information, tasks and decisions."
    );
    ImGui::Separator();
    ImGui::Spacing();

    if (gDocuments.empty())
    {
        ImGui::TextWrapped(
            "Your document library is empty. Add a PDF, DOCX or TXT file from My Documents first."
        );
        ImGui::EndChild();
        return;
    }

    if (gSelectedKeyInfoDocumentId == -1 ||
        std::none_of(
            gDocuments.begin(), gDocuments.end(),
            [](const Document& item) { return item.id == gSelectedKeyInfoDocumentId; }))
    {
        gSelectedKeyInfoDocumentId = gDocuments.front().id;
    }

    const Document* selectedDocument = nullptr;
    for (const Document& item : gDocuments)
    {
        if (item.id == gSelectedKeyInfoDocumentId)
        {
            selectedDocument = &item;
            break;
        }
    }

    const char* preview = selectedDocument ? selectedDocument->name.c_str() : "Select a document";
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("Document", preview))
    {
        for (const Document& item : gDocuments)
        {
            bool isSelected = item.id == gSelectedKeyInfoDocumentId;
            if (ImGui::Selectable(item.name.c_str(), isSelected))
            {
                gSelectedKeyInfoDocumentId = item.id;
                gKeyInformationReport.clear();
                gExtractedDocumentText.clear();
                gExtractedDocumentName.clear();
            }
            if (isSelected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    bool documentLoaded = selectedDocument &&
        !gExtractedDocumentText.empty() &&
        gExtractedDocumentName == selectedDocument->name;

    if (ImGui::Button(
        documentLoaded ? "Reload Selected Document" : "Load Selected Document",
        ImVec2(220, 38)))
    {
        if (selectedDocument)
        {
            gKeyInformationReport.clear();
            if (ExtractDocumentText(*selectedDocument))
            {
                gOpenReaderPopup = false;
                statusMessage = "Document loaded. Ready to extract key information.";
            }
        }
    }

    ImGui::SameLine();
    if (documentLoaded)
        ImGui::TextDisabled("Loaded: %s", gExtractedDocumentName.c_str());
    else
        ImGui::TextDisabled("Load a document to continue.");

    ImGui::Spacing();
    if (!documentLoaded)
        ImGui::BeginDisabled();

    if (ImGui::Button("Extract Key Information", ImVec2(230, 42)))
    {
        ExtractKeyInformation(gExtractedDocumentName, gExtractedDocumentText);
    }

    if (!documentLoaded)
        ImGui::EndDisabled();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::Text("Extraction Results");
    ImGui::SameLine();
    if (gKeyInformationReport.empty())
        ImGui::BeginDisabled();
    if (ImGui::Button("Save Key Information Report"))
    {
        std::string suggestedName = "key_information_report.txt";
        if (!gExtractedDocumentName.empty())
        {
            fs::path sourceName(gExtractedDocumentName);
            suggestedName = sourceName.stem().string() + "_key_information.txt";
        }
        SaveTextReport(
            "Save Key Information Report",
            suggestedName,
            gKeyInformationReport
        );
    }
    if (gKeyInformationReport.empty())
        ImGui::EndDisabled();

    ImGui::SameLine();
    if (gKeyInformationReport.empty())
        ImGui::BeginDisabled();
    if (ImGui::Button("Export Key Information PDF"))
    {
        std::string suggestedName = "key_information_report.pdf";
        if (!gExtractedDocumentName.empty())
            suggestedName = fs::path(gExtractedDocumentName).stem().string() + "_key_information.pdf";
        SavePdfReport(
            "Export Key Information as PDF",
            suggestedName,
            "Key Information Report",
            gKeyInformationReport
        );
    }
    if (gKeyInformationReport.empty())
        ImGui::EndDisabled();

    ImGui::BeginChild("KeyInformationResults", ImVec2(0, 360), true);

    if (gKeyInformationReport.empty())
    {
        ImGui::TextDisabled(
            "Your extracted information will appear here. Select a document, load it, and click Extract Key Information."
        );
    }
    else
    {
        ImGui::PushTextWrapPos(
            ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x
        );
        ImGui::TextUnformatted(gKeyInformationReport.c_str());
        ImGui::PopTextWrapPos();
    }

    ImGui::EndChild();
    ImGui::Spacing();
    ImGui::TextWrapped("%s", statusMessage.c_str());
    ImGui::EndChild();
}


void DrawDocumentComparisonPage()
{
    ImGui::BeginChild("DocumentComparisonWorkspace", ImVec2(GetMainContentWidth(), 0), true);

    ImGui::Text("AI Document Comparison");
    ImGui::TextDisabled("Compare two documents and identify similarities, changes and possible conflicts.");
    ImGui::Separator();
    ImGui::Spacing();

    if (gDocuments.size() < 2)
    {
        ImGui::TextWrapped("Add at least two documents in My Documents before comparing them.");
        ImGui::EndChild();
        return;
    }

    auto findDocument = [](int id) -> const Document* {
        for (const Document& document : gDocuments)
            if (document.id == id)
                return &document;
        return nullptr;
    };

    if (!findDocument(gSelectedCompareDocumentA))
        gSelectedCompareDocumentA = gDocuments[0].id;
    if (!findDocument(gSelectedCompareDocumentB) || gSelectedCompareDocumentB == gSelectedCompareDocumentA)
    {
        gSelectedCompareDocumentB = -1;
        for (const Document& document : gDocuments)
        {
            if (document.id != gSelectedCompareDocumentA)
            {
                gSelectedCompareDocumentB = document.id;
                break;
            }
        }
    }

    const Document* docA = findDocument(gSelectedCompareDocumentA);
    const Document* docB = findDocument(gSelectedCompareDocumentB);

    ImGui::Text("First document");
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##CompareDocumentA", docA ? docA->name.c_str() : "Select first document"))
    {
        for (const Document& document : gDocuments)
        {
            bool selected = document.id == gSelectedCompareDocumentA;
            if (ImGui::Selectable(document.name.c_str(), selected))
            {
                gSelectedCompareDocumentA = document.id;
                gDocumentComparisonReport.clear();
                if (gSelectedCompareDocumentB == gSelectedCompareDocumentA)
                {
                    gSelectedCompareDocumentB = -1;
                    for (const Document& candidate : gDocuments)
                        if (candidate.id != gSelectedCompareDocumentA) { gSelectedCompareDocumentB = candidate.id; break; }
                }
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    docA = findDocument(gSelectedCompareDocumentA);
    docB = findDocument(gSelectedCompareDocumentB);

    ImGui::Spacing();
    ImGui::Text("Second document");
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##CompareDocumentB", docB ? docB->name.c_str() : "Select second document"))
    {
        for (const Document& document : gDocuments)
        {
            if (document.id == gSelectedCompareDocumentA)
                continue;
            bool selected = document.id == gSelectedCompareDocumentB;
            if (ImGui::Selectable(document.name.c_str(), selected))
            {
                gSelectedCompareDocumentB = document.id;
                gDocumentComparisonReport.clear();
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    ImGui::Spacing();
    ImGui::TextWrapped("Both documents are read from your existing library. Their files and database records will not be changed.");
    ImGui::Spacing();

    if (!docA || !docB || docA->id == docB->id)
        ImGui::BeginDisabled();

    if (ImGui::Button("Compare Documents", ImVec2(220, 42)))
    {
        gDocumentComparisonReport.clear();
        std::string nameA = docA->name;
        std::string nameB = docB->name;
        statusMessage = "Loading the first document...";

        if (ExtractDocumentText(*docA))
        {
            std::string textA = gExtractedDocumentText;
            statusMessage = "Loading the second document...";
            if (ExtractDocumentText(*docB))
            {
                std::string textB = gExtractedDocumentText;
                CompareDocumentTexts(nameA, textA, nameB, textB);
            }
        }
    }

    if (!docA || !docB || docA->id == docB->id)
        ImGui::EndDisabled();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::Text("Comparison Report");
    ImGui::SameLine();
    if (gDocumentComparisonReport.empty())
        ImGui::BeginDisabled();
    if (ImGui::Button("Save Comparison Report"))
    {
        std::string suggestedName = "document_comparison_report.txt";
        if (docA && docB)
        {
            suggestedName =
                fs::path(docA->name).stem().string() + "_vs_" +
                fs::path(docB->name).stem().string() + "_comparison.txt";
        }
        SaveTextReport(
            "Save Document Comparison Report",
            suggestedName,
            gDocumentComparisonReport
        );
    }
    if (gDocumentComparisonReport.empty())
        ImGui::EndDisabled();

    ImGui::SameLine();
    if (gDocumentComparisonReport.empty())
        ImGui::BeginDisabled();
    if (ImGui::Button("Export Comparison PDF"))
    {
        std::string suggestedName = "document_comparison_report.pdf";
        if (docA && docB)
        {
            suggestedName =
                fs::path(docA->name).stem().string() + "_vs_" +
                fs::path(docB->name).stem().string() + "_comparison.pdf";
        }
        SavePdfReport(
            "Export Document Comparison as PDF",
            suggestedName,
            "Document Comparison Report",
            gDocumentComparisonReport
        );
    }
    if (gDocumentComparisonReport.empty())
        ImGui::EndDisabled();

    ImGui::BeginChild("DocumentComparisonResults", ImVec2(0, 360), true);
    if (gDocumentComparisonReport.empty())
    {
        ImGui::TextDisabled("Your comparison report will appear here after you select two documents and click Compare Documents.");
    }
    else
    {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
        ImGui::TextUnformatted(gDocumentComparisonReport.c_str());
        ImGui::PopTextWrapPos();
    }
    ImGui::EndChild();
    ImGui::Spacing();
    ImGui::TextWrapped("%s", statusMessage.c_str());
    ImGui::EndChild();
}


void DrawExportCenterPage()
{
    ImGui::BeginChild("ExportCenterWorkspace", ImVec2(GetMainContentWidth(), 0), true);
    ImGui::Text("Advanced Export Center");
    ImGui::TextDisabled("Export your document library, AI reports and summary history.");
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::Text("Document Library");
    ImGui::TextWrapped("Create a spreadsheet containing document names, file types, sizes, dates, categories, tags and stored paths.");
    ImGui::TextDisabled("Documents available: %d", static_cast<int>(gDocuments.size()));
    if (ImGui::Button("Export Document Inventory (CSV)", ImVec2(280, 38)))
    {
        SaveCsvReport("Export Document Inventory", "documind_document_inventory.csv", BuildDocumentInventoryCsv());
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::Text("AI Summary History");
    ImGui::TextWrapped("Export saved summaries with their document names, summary types, generation dates and full summary text.");
    ImGui::TextDisabled("Saved summaries: %d", static_cast<int>(gSummaries.size()));
    if (ImGui::Button("Export Summary History (CSV)", ImVec2(280, 38)))
    {
        SaveCsvReport("Export Summary History", "documind_summary_history.csv", BuildSummaryHistoryCsv());
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::Text("Current AI Reports");
    ImGui::TextDisabled("Export the reports currently loaded in the Key Information and Document Comparison pages.");
    ImGui::BeginDisabled(gKeyInformationReport.empty());
    if (ImGui::Button("Key Information - TXT", ImVec2(190, 36)))
        SaveTextReport("Export Key Information", "key_information_report.txt", gKeyInformationReport);
    ImGui::SameLine();
    if (ImGui::Button("Key Information - PDF", ImVec2(190, 36)))
        SavePdfReport("Export Key Information PDF", "key_information_report.pdf", "Key Information Report", gKeyInformationReport);
    ImGui::EndDisabled();

    ImGui::Spacing();
    ImGui::BeginDisabled(gDocumentComparisonReport.empty());
    if (ImGui::Button("Comparison Report - TXT", ImVec2(190, 36)))
        SaveTextReport("Export Comparison Report", "document_comparison_report.txt", gDocumentComparisonReport);
    ImGui::SameLine();
    if (ImGui::Button("Comparison Report - PDF", ImVec2(190, 36)))
        SavePdfReport("Export Comparison PDF", "document_comparison_report.pdf", "Document Comparison Report", gDocumentComparisonReport);
    ImGui::EndDisabled();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextWrapped("CSV files can be opened in Microsoft Excel, LibreOffice Calc and other spreadsheet applications. Exporting creates a separate file and does not modify your original documents or DocuMind database.");
    ImGui::Spacing();
    ImGui::TextWrapped("%s", statusMessage.c_str());
    ImGui::EndChild();
}

void DrawDuplicateDetectorPage()
{
    ImGui::BeginChild("DuplicateDetectorWorkspace", ImVec2(GetMainContentWidth(), 0), true);

    ImGui::Text("Duplicate Document Detector");
    ImGui::TextDisabled("Find identical files in your DocuMind library without deleting or changing anything.");
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::TextWrapped("DocuMind compares file sizes and content fingerprints, then checks matching candidates byte-for-byte. This avoids treating files as duplicates just because they have the same name.");
    ImGui::Spacing();

    if (ImGui::Button("Scan My Documents", ImVec2(210, 38)))
        ScanForDuplicateDocuments();

    ImGui::SameLine();
    ImGui::TextDisabled("%d documents in library", static_cast<int>(gDocuments.size()));

    if (gDuplicateScanCompleted)
    {
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Text("Scan results");
        ImGui::Text("Files checked: %d", gDuplicateFilesChecked);
        ImGui::SameLine(220.0f);
        ImGui::Text("Duplicate groups: %d", static_cast<int>(gDuplicateGroups.size()));
        ImGui::SameLine(440.0f);
        ImGui::Text("Skipped: %d", gDuplicateMissingFiles);

        int duplicateFileCount = 0;
        long long duplicateBytes = 0;
        for (const DuplicateDocumentGroup& group : gDuplicateGroups)
        {
            duplicateFileCount += static_cast<int>(group.documents.size());
            for (size_t i = 1; i < group.documents.size(); ++i)
                duplicateBytes += group.documents[i].size;
        }

        ImGui::Text("Files involved in duplicate groups: %d", duplicateFileCount);
        ImGui::Text("Potential extra storage occupied: %s", FormatFileSize(duplicateBytes).c_str());
        ImGui::Spacing();

        if (gDuplicateGroups.empty())
        {
            ImGui::TextColored(ImVec4(0.2f, 0.65f, 0.35f, 1.0f),
                "No identical documents were found in the current library.");
        }
        else
        {
            for (size_t groupIndex = 0; groupIndex < gDuplicateGroups.size(); ++groupIndex)
            {
                const DuplicateDocumentGroup& group = gDuplicateGroups[groupIndex];
                ImGui::PushID(static_cast<int>(groupIndex) + 9000);
                ImGui::BeginChild("DuplicateGroup", ImVec2(0, 125.0f + 34.0f * group.documents.size()), true);
                ImGui::Text("Duplicate group %d - %d identical files",
                    static_cast<int>(groupIndex + 1), static_cast<int>(group.documents.size()));
                ImGui::Separator();

                for (const Document& document : group.documents)
                {
                    ImGui::PushID(document.id);
                    ImGui::Text("%s", document.name.c_str());
                    ImGui::SameLine();
                    ImGui::TextDisabled("| %s | %s", FormatFileSize(document.size).c_str(),
                        document.categoryName.c_str());
                    if (ImGui::SmallButton("View in My Documents"))
                        DashboardOpenDocument(document);
                    ImGui::PopID();
                }
                ImGui::EndChild();
                ImGui::PopID();
                ImGui::Spacing();
            }
        }
    }
    else
    {
        ImGui::Spacing();
        ImGui::TextDisabled("Run a scan to check your library for identical files.");
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextWrapped("Safety: this tool only reads files during scanning. It does not delete documents, change filenames, or modify database records. Review duplicates manually in My Documents before taking any action.");
    ImGui::Spacing();
    ImGui::TextWrapped("%s", statusMessage.c_str());
    ImGui::EndChild();
}

void DrawAppearancePage()
{
    ImGui::BeginChild(
        "AppearanceWorkspace",
        ImVec2(GetMainContentWidth(), 0),
        true,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse
    );

    // Keep the page simple: one heading, one description and one theme card.
    ImGui::Text("Appearance");
    ImGui::TextDisabled("Personalize the appearance of DocuMind AI.");
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // The card is deliberately tall enough for all its contents, and its
    // scrollbar is disabled so the theme controls remain fully visible.
    ImGui::BeginChild(
        "AppearanceCard",
        ImVec2(0, 230),
        true,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse
    );

    ImGui::Text("Application theme");
    ImGui::Spacing();
    ImGui::TextWrapped(
        "Choose a theme for your workspace. Your choice will be saved "
        "and restored the next time you open DocuMind AI."
    );
    ImGui::Spacing();
    ImGui::Spacing();

    const float availableWidth = ImGui::GetContentRegionAvail().x;
    const float buttonGap = ImGui::GetStyle().ItemSpacing.x;
    const float buttonWidth = std::max(
        130.0f,
        (availableWidth - buttonGap) * 0.5f
    );

    // Keep both controls active. Selecting the current theme is harmless
    // and avoids accidentally disabling the option the user wants to choose.
    if (ImGui::Button("Light Mode", ImVec2(buttonWidth, 44)))
    {
        gDarkTheme = false;
        ApplyCurrentTheme();
        SaveAppearancePreference();
        statusMessage = "Light mode enabled and saved.";
    }

    ImGui::SameLine();

    if (ImGui::Button("Dark Mode", ImVec2(buttonWidth, 44)))
    {
        gDarkTheme = true;
        ApplyCurrentTheme();
        SaveAppearancePreference();
        statusMessage = "Dark mode enabled and saved.";
    }

    ImGui::Spacing();
    ImGui::Spacing();
    ImGui::Text(
        "Current theme: %s",
        gDarkTheme ? "Dark" : "Light"
    );

    ImGui::EndChild();

    ImGui::Spacing();
    ImGui::TextWrapped("%s", statusMessage.c_str());

    ImGui::EndChild();
}

void DrawCurrentPage()
{
    switch (selectedPage)
    {
    case 0:
        DrawDashboard();
        break;

    case 1:
        DrawDocumentsPage();
        break;

    case 2:
        DrawAIAssistantPage();
        break;

    case 3:
        DrawKeyInformationPage();
        break;

    case 4:
        DrawDocumentComparisonPage();
        break;

    case 5:
        DrawSummariesPage();
        break;

    case 6:
        DrawExportCenterPage();
        break;

    case 7:
        DrawAppearancePage();
        break;

    case 8:
        DrawDuplicateDetectorPage();
        break;

    default:
        DrawDashboard();
        break;
    }
}

void DrawAssistant()
{
    ImGui::BeginChild(
        "AssistantPanel",
        ImVec2(kAssistantWidth, 0),
        true
    );

    ImGui::Text("AI Assistant");

    ImGui::TextDisabled(
        "Your document intelligence companion"
    );

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::TextWrapped(
        "Chat with a selected document and generate summaries using your local AI."
    );

    ImGui::Spacing();

    ImGui::Text("Suggested prompts");

    ImGui::Spacing();

    if (ImGui::Button(
        "Summarise a document",
        ImVec2(-1, 38)))
    {
        selectedPage = 1;
        statusMessage =
            "Choose Extract on a document, then generate a local AI summary.";
    }

    if (ImGui::Button(
        "Find key information",
        ImVec2(-1, 38)))
    {
        selectedPage = 2;
        statusMessage =
            "Load a document in AI Assistant to ask questions about it.";
    }

    if (ImGui::Button(
        "Extract dates and names",
        ImVec2(-1, 38)))
    {
        selectedPage = 2;
        statusMessage =
            "Ask DocuMind about dates, names and key details in a loaded document.";
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::Text("Ask DocuMind");

    static char question[512] = "";

    ImGui::InputTextMultiline(
        "##Question",
        question,
        sizeof(question),
        ImVec2(-1, 100)
    );

    if (ImGui::Button(
        "Send question",
        ImVec2(-1, 40)))
    {
        if (question[0] == '\0')
        {
            statusMessage =
                "Please enter a question first.";
        }
        else if (gDocuments.empty())
        {
            statusMessage =
                "Upload a document before asking questions.";
        }
        else
        {
            selectedPage = 2;
            statusMessage =
                "Select and load a document in the AI Assistant workspace before sending your question.";
        }
    }

    ImGui::Spacing();

    ImGui::TextWrapped(
        "Choose AI Assistant from the sidebar to chat with a selected document. "
        "Your questions are answered by the local Qwen model using that document's text."
    );

    ImGui::EndChild();
}

int main(int, char**)
{
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0)
    {
        SDL_ShowSimpleMessageBox(
            SDL_MESSAGEBOX_ERROR,
            "DocuMind AI",
            SDL_GetError(),
            nullptr
        );

        return 1;
    }

    int windowW = 1280;
    int windowH = 720;

    SDL_Rect usable;

    if (SDL_GetDisplayUsableBounds(0, &usable) == 0)
    {
        windowW = std::min(
            1440,
            static_cast<int>(usable.w * 0.90f)
        );

        windowH = std::min(
            900,
            static_cast<int>(usable.h * 0.85f)
        );
    }

    SDL_Window* window = SDL_CreateWindow(
        "DocuMind AI - Document Intelligence",
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        windowW,
        windowH,
        SDL_WINDOW_SHOWN |
        SDL_WINDOW_RESIZABLE |
        SDL_WINDOW_ALLOW_HIGHDPI
    );

    if (!window)
    {
        SDL_ShowSimpleMessageBox(
            SDL_MESSAGEBOX_ERROR,
            "Window Error",
            SDL_GetError(),
            nullptr
        );

        SDL_Quit();
        return 1;
    }

    SDL_SetWindowMinimumSize(
        window,
        1000,
        600
    );

    SDL_Renderer* renderer = SDL_CreateRenderer(
        window,
        -1,
        SDL_RENDERER_ACCELERATED |
        SDL_RENDERER_PRESENTVSYNC
    );

    if (!renderer)
    {
        renderer = SDL_CreateRenderer(
            window,
            -1,
            SDL_RENDERER_SOFTWARE
        );
    }

    if (!renderer)
    {
        SDL_ShowSimpleMessageBox(
            SDL_MESSAGEBOX_ERROR,
            "Renderer Error",
            SDL_GetError(),
            window
        );

        SDL_DestroyWindow(window);
        SDL_Quit();

        return 1;
    }

    IMGUI_CHECKVERSION();

    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();

    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    LoadFonts(io);
    ApplyLightTheme();

    ImGui_ImplSDL2_InitForSDLRenderer(
        window,
        renderer
    );

    ImGui_ImplSDLRenderer2_Init(renderer);

    if (InitializeDatabase())
    {
        LoadCategories();
        LoadDocuments();
        LoadSummaries();
        LoadChatConversations();
        statusMessage =
            "DocuMind AI is ready. Your documents, summaries and saved chat history are loaded.";
    }
    else
    {
        statusMessage =
            "Database initialization failed. Check the database folder.";
    }

    LoadAppearancePreference();
    ApplyCurrentTheme();

    bool running = true;

    while (running)
    {
        SDL_Event event;

        while (SDL_PollEvent(&event))
        {
            ImGui_ImplSDL2_ProcessEvent(&event);

            if (event.type == SDL_QUIT)
                running = false;

            if (event.type == SDL_WINDOWEVENT &&
                event.window.event == SDL_WINDOWEVENT_CLOSE &&
                event.window.windowID == SDL_GetWindowID(window))
            {
                running = false;
            }
        }

        ImGui_ImplSDLRenderer2_NewFrame();
        ImGui_ImplSDL2_NewFrame();

        ImGui::NewFrame();

        ImGui::SetNextWindowPos(
            ImVec2(0, 0)
        );

        ImGui::SetNextWindowSize(
            io.DisplaySize
        );

        ImGui::Begin(
            "DocuMind Main Window",
            nullptr,
            ImGuiWindowFlags_NoTitleBar |
            ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoBringToFrontOnFocus
        );

        DrawSidebar();

        ImGui::SameLine();

        DrawCurrentPage();

        ImGui::SameLine();

        DrawAssistant();

        ImGui::End();

        ImGui::Render();

        SDL_SetRenderDrawColor(
            renderer,
            245,
            247,
            250,
            255
        );

        SDL_RenderClear(renderer);

        ImGui_ImplSDLRenderer2_RenderDrawData(
            ImGui::GetDrawData(),
            renderer
        );

        SDL_RenderPresent(renderer);
    }

    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();

    ImGui::DestroyContext();

    if (gDatabase)
    {
        sqlite3_close(gDatabase);
        gDatabase = nullptr;
    }

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);

    SDL_Quit();

    return 0;
}
