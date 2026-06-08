#include "DossierDatabase.hpp"
#include <iostream>
#include <cmath>

DossierDatabase::DossierDatabase(const std::string& dbPath) : m_dbPath(dbPath) {}

DossierDatabase::~DossierDatabase() {
    if (m_db) {
        sqlite3_close(m_db);
    }
}

bool DossierDatabase::init() {
    std::lock_guard<std::mutex> lock(m_mutex);
    int rc = sqlite3_open(m_dbPath.c_str(), &m_db);
    if (rc) {
        std::cerr << "[DossierDatabase] Can't open database: " << sqlite3_errmsg(m_db) << std::endl;
        return false;
    }

    const char* schema = 
        "CREATE TABLE IF NOT EXISTS Entities ("
        "uuid TEXT PRIMARY KEY,"
        "type TEXT,"
        "embedding BLOB,"
        "first_seen TEXT,"
        "last_seen TEXT,"
        "dossier_text TEXT,"
        "sightings_count INTEGER DEFAULT 0"
        ");"
        "CREATE TABLE IF NOT EXISTS Sightings ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "entity_uuid TEXT,"
        "timestamp DATETIME DEFAULT CURRENT_TIMESTAMP,"
        "session_id TEXT,"
        "FOREIGN KEY(entity_uuid) REFERENCES Entities(uuid)"
        ");"
        "CREATE TABLE IF NOT EXISTS Faces ("
        "id INTEGER PRIMARY KEY,"
        "name TEXT,"
        "embedding BLOB"
        ");";

    char* errMsg = nullptr;
    rc = sqlite3_exec(m_db, schema, nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
        std::cerr << "[DossierDatabase] SQL error during init: " << errMsg << std::endl;
        sqlite3_free(errMsg);
        return false;
    }

    return true;
}

bool DossierDatabase::upsertEntity(const DossierEntry& entry) {
    std::lock_guard<std::mutex> lock(m_mutex);
    const char* sql = "INSERT OR REPLACE INTO Entities (uuid, type, embedding, first_seen, last_seen, dossier_text, sightings_count) "
                      "VALUES (?, ?, ?, ?, ?, ?, ?);";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;

    sqlite3_bind_text(stmt, 1, entry.uuid.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, entry.type.c_str(), -1, SQLITE_TRANSIENT);
    
    std::string blob = vectorToBlob(entry.embedding);
    sqlite3_bind_blob(stmt, 3, blob.data(), static_cast<int>(blob.size()), SQLITE_TRANSIENT);
    
    sqlite3_bind_text(stmt, 4, entry.first_seen.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, entry.last_seen.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, entry.dossier_text.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 7, entry.sightings_count);

    bool success = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    return success;
}

bool DossierDatabase::getEntityByUUID(const std::string& uuid, DossierEntry& outEntry) {
    std::lock_guard<std::mutex> lock(m_mutex);
    const char* sql = "SELECT uuid, type, embedding, first_seen, last_seen, dossier_text, sightings_count FROM Entities WHERE uuid = ?;";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;

    sqlite3_bind_text(stmt, 1, uuid.c_str(), -1, SQLITE_TRANSIENT);

    bool found = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        outEntry.uuid = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        outEntry.type = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        
        const void* blob = sqlite3_column_blob(stmt, 2);
        int bytes = sqlite3_column_bytes(stmt, 2);
        outEntry.embedding = blobToVector(blob, bytes);
        
        outEntry.first_seen = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        outEntry.last_seen = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        outEntry.dossier_text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
        outEntry.sightings_count = sqlite3_column_int(stmt, 6);
        found = true;
    }

    sqlite3_finalize(stmt);
    return found;
}

bool DossierDatabase::findNearestEntity(const std::vector<float>& embedding, const std::string& type, float threshold, DossierEntry& outEntry) {
    std::lock_guard<std::mutex> lock(m_mutex);
    const char* sql = "SELECT uuid, type, embedding, first_seen, last_seen, dossier_text, sightings_count FROM Entities WHERE type = ?;";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;

    sqlite3_bind_text(stmt, 1, type.c_str(), -1, SQLITE_TRANSIENT);

    float bestSim = -1.0f;
    bool found = false;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const void* blob = sqlite3_column_blob(stmt, 2);
        int bytes = sqlite3_column_bytes(stmt, 2);
        std::vector<float> dbEmbedding = blobToVector(blob, bytes);

        if (embedding.size() != dbEmbedding.size()) continue;

        // Cosine Similarity
        float dot = 0.0f, normA = 0.0f, normB = 0.0f;
        for (size_t i = 0; i < embedding.size(); ++i) {
            dot += embedding[i] * dbEmbedding[i];
            normA += embedding[i] * embedding[i];
            normB += dbEmbedding[i] * dbEmbedding[i];
        }
        float sim = dot / (std::sqrt(normA) * std::sqrt(normB) + 1e-10f);

        if (sim > bestSim && sim > threshold) {
            bestSim = sim;
            outEntry.uuid = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            outEntry.type = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            outEntry.embedding = dbEmbedding;
            outEntry.first_seen = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
            outEntry.last_seen = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
            outEntry.dossier_text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
            outEntry.sightings_count = sqlite3_column_int(stmt, 6);
            found = true;
        }
    }

    sqlite3_finalize(stmt);
    return found;
}

bool DossierDatabase::updateDossierText(const std::string& uuid, const std::string& text) {
    std::lock_guard<std::mutex> lock(m_mutex);
    const char* sql = "UPDATE Entities SET dossier_text = ? WHERE uuid = ?;";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;

    sqlite3_bind_text(stmt, 1, text.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, uuid.c_str(), -1, SQLITE_TRANSIENT);

    bool success = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    return success;
}

bool DossierDatabase::addSighting(const std::string& entityUuid, const std::string& sessionId) {
    std::lock_guard<std::mutex> lock(m_mutex);
    // Add sighting
    const char* sqlSighting = "INSERT INTO Sightings (entity_uuid, session_id) VALUES (?, ?);";
    sqlite3_stmt* stmtS;
    if (sqlite3_prepare_v2(m_db, sqlSighting, -1, &stmtS, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmtS, 1, entityUuid.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmtS, 2, sessionId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmtS);
    sqlite3_finalize(stmtS);

    // Update last_seen and count in Entities
    const char* sqlUpdate = "UPDATE Entities SET last_seen = CURRENT_TIMESTAMP, sightings_count = sightings_count + 1 WHERE uuid = ?;";
    sqlite3_stmt* stmtU;
    if (sqlite3_prepare_v2(m_db, sqlUpdate, -1, &stmtU, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmtU, 1, entityUuid.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmtU);
    sqlite3_finalize(stmtU);

    return true;
}

int DossierDatabase::getEntityCount() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    const char* sql = "SELECT COUNT(*) FROM Entities;";
    sqlite3_stmt* stmt;
    int count = 0;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            count = sqlite3_column_int(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }
    return count;
}

std::vector<DossierEntry> DossierDatabase::getAllEntities() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<DossierEntry> results;
    const char* sql = "SELECT uuid, type, embedding, first_seen, last_seen, dossier_text, sightings_count FROM Entities ORDER BY last_seen DESC;";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            DossierEntry entry;
            entry.uuid = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            entry.type = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            
            const void* blob = sqlite3_column_blob(stmt, 2);
            int bytes = sqlite3_column_bytes(stmt, 2);
            entry.embedding = blobToVector(blob, bytes);
            
            entry.first_seen = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
            entry.last_seen = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
            entry.dossier_text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
            entry.sightings_count = sqlite3_column_int(stmt, 6);
            results.push_back(entry);
        }
        sqlite3_finalize(stmt);
    }
    return results;
}

bool DossierDatabase::upsertFace(int id, const std::string& name, const std::vector<float>& embedding) {
    std::lock_guard<std::mutex> lock(m_mutex);
    const char* sql = "INSERT OR REPLACE INTO Faces (id, name, embedding) VALUES (?, ?, ?);";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;

    sqlite3_bind_int(stmt, 1, id);
    sqlite3_bind_text(stmt, 2, name.c_str(), -1, SQLITE_TRANSIENT);
    std::string blob = vectorToBlob(embedding);
    sqlite3_bind_blob(stmt, 3, blob.data(), static_cast<int>(blob.size()), SQLITE_TRANSIENT);

    bool success = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    return success;
}

bool DossierDatabase::updateFaceName(int id, const std::string& name) {
    std::lock_guard<std::mutex> lock(m_mutex);
    const char* sql = "UPDATE Faces SET name = ? WHERE id = ?;";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;

    sqlite3_bind_text(stmt, 1, name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, id);

    bool success = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    return success;
}

std::vector<FaceRecord> DossierDatabase::getAllFaces() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<FaceRecord> results;
    const char* sql = "SELECT id, name, embedding FROM Faces ORDER BY id ASC;";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            FaceRecord rec;
            rec.id = sqlite3_column_int(stmt, 0);
            const unsigned char* nameText = sqlite3_column_text(stmt, 1);
            if (nameText) rec.name = reinterpret_cast<const char*>(nameText);
            const void* blob = sqlite3_column_blob(stmt, 2);
            int bytes = sqlite3_column_bytes(stmt, 2);
            rec.embedding = blobToVector(blob, bytes);
            results.push_back(std::move(rec));
        }
        sqlite3_finalize(stmt);
    }
    return results;
}

int DossierDatabase::getMaxFaceId() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    const char* sql = "SELECT MAX(id) FROM Faces;";
    sqlite3_stmt* stmt;
    int maxId = 0;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            maxId = sqlite3_column_int(stmt, 0); // NULL -> 0
        }
        sqlite3_finalize(stmt);
    }
    return maxId;
}

std::vector<float> DossierDatabase::blobToVector(const void* blob, int size) {
    int count = size / sizeof(float);
    const float* data = reinterpret_cast<const float*>(blob);
    return std::vector<float>(data, data + count);
}

std::string DossierDatabase::vectorToBlob(const std::vector<float>& vec) {
    return std::string(reinterpret_cast<const char*>(vec.data()), vec.size() * sizeof(float));
}
