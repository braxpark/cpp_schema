#include "include/src/pgfe/data.hpp"
#include "include/src/pgfe/exceptions.hpp"
#include "include/src/pgfe/pgfe.hpp"
#include <chrono>
#include <cstdio>
#include <format>
#include <fstream>
#include <iostream>
#include <queue>
#include <unordered_set>
#include <vector>
#include <map>
#include "struct_mapping/struct_mapping.h"

namespace pgfe = dmitigr::pgfe;

template<typename... Args>
std::string formattedString(const std::format_string<Args...> fmt, Args&&... args) {
    return std::vformat(fmt.get(), std::make_format_args(args...));
}

struct DatabaseInfo {
    std::string host;
    std::string dbName;
    std::string username;
    std::string password;
};

void parseFileIntoConfig(const std::string& fileName, DatabaseInfo& config) {
    // Assume file exists and is accessible
    std::ifstream ifs(fileName);
    std::string content( (std::istreambuf_iterator<char>(ifs) ), (std::istreambuf_iterator<char>()));
    std::stringstream ssContent(content);
    struct_mapping::reg(&DatabaseInfo::host, "host");
    struct_mapping::reg(&DatabaseInfo::dbName, "dbName");
    struct_mapping::reg(&DatabaseInfo::username, "username");
    struct_mapping::reg(&DatabaseInfo::password, "password");
    struct_mapping::map_json_to_struct(config, ssContent);
}

std::string getChildrenQuery = R"(SELECT
        tc.table_schema, 
        tc.constraint_name, 
        tc.table_name as "tableName", 
        kcu.column_name, 
        ccu.table_schema AS foreign_table_schema,
        ccu.table_name AS foreign_table_name,
        ccu.column_name AS foreign_column_name 
        FROM information_schema.table_constraints AS tc 
        JOIN information_schema.key_column_usage AS kcu
        ON tc.constraint_name = kcu.constraint_name
        AND tc.table_schema = kcu.table_schema
        JOIN information_schema.constraint_column_usage AS ccu
        ON ccu.constraint_name = tc.constraint_name
        WHERE tc.constraint_type = 'FOREIGN KEY'
        AND tc.table_schema='public'
        AND ccu.table_name =')";

std::string getSupportersQuery = R"(SELECT
        tc.table_schema, 
        tc.constraint_name, 
        tc.table_name as "tableName", 
        kcu.column_name, 
        ccu.table_schema AS foreign_table_schema,
        ccu.table_name AS foreign_table_name,
        ccu.column_name AS foreign_column_name 
        FROM information_schema.table_constraints AS tc 
        JOIN information_schema.key_column_usage AS kcu
        ON tc.constraint_name = kcu.constraint_name
        AND tc.table_schema = kcu.table_schema
        JOIN information_schema.constraint_column_usage AS ccu
        ON ccu.constraint_name = tc.constraint_name
        WHERE tc.constraint_type = 'FOREIGN KEY'
        AND tc.table_schema='public'
        AND tc.table_name =')";

std::string getTableFieldsAndDataTypes(const std::string& tableName) {
    return R"(
        SELECT column_name, is_nullable, data_type
        FROM information_schema.columns WHERE table_name = ')" + tableName + "'";
}

std::string valuesFromVector(std::vector<std::string> vec, std::string delimiter = ",") {
    std::stringstream s;
    copy(vec.begin(), vec.end(), std::ostream_iterator<std::string>(s, ","));
    return s.str().substr(0, s.str().length() - 1);
}

std::string getRowsByFKEYQuery(const std::string& tableName, const std::string& colName, const std::string& colValue, bool isString = false, const std::string& where = "") {
    const std::string value = isString ? "'" + colValue + "'" : colValue;
    return (R"(
        SELECT
            *
        FROM

    )" + 
    tableName + 
    R"(
        WHERE   
    )" +
    colName + 
    R"(
     = 
    )" + 
    value +
    where
    );
}

std::string getSupporterQuery(const std::string& tableName) {
    return getSupportersQuery + tableName + "'";
}

struct sortDepListOnDependencySize {
    inline bool operator()(std::pair<std::string, std::unordered_set<std::string>>& a, std::pair<std::string, std::unordered_set<std::string>>& b) {
        return a.second.size() < b.second.size();
    }
};

std::string getForeignKeyQuery(const std::string& tableName) {
    return getChildrenQuery + tableName + "'";
}

enum PGDataType { NUMERIC, INTEGER, BIGINT, BOOLEAN, CHARACTERVARYING, TEXT, JSONB, TIMESTAMPNOTIMEZONE, DATE, OTHER };

PGDataType getPGDataType(const std::string& dataType) {
    if(dataType == "integer") return PGDataType::INTEGER;
    else if(dataType == "bigint") return PGDataType::BIGINT;
    else if(dataType == "numeric") return PGDataType::NUMERIC;
    else if(dataType == "boolean") return PGDataType::BOOLEAN;
    else if(dataType == "character varying") return PGDataType::CHARACTERVARYING;
    else if(dataType == "text") return PGDataType::TEXT;
    else if(dataType == "jsonb") return PGDataType::JSONB;
    else if(dataType == "timestamp without time zone") return PGDataType::TIMESTAMPNOTIMEZONE;
    else if(dataType == "date") return PGDataType::DATE;
    return PGDataType::OTHER;
}

bool pgDataTypeNeedsEnclosedQuotes(const PGDataType& dataType) {
    std::vector<PGDataType> encloseds = { PGDataType::CHARACTERVARYING, PGDataType::TEXT, PGDataType::JSONB, PGDataType::TIMESTAMPNOTIMEZONE, PGDataType::DATE, PGDataType::OTHER };
    for(auto& dt : encloseds) {
        if(dt == dataType) return true;
    }
    return false;
}

struct ColInfo {
    bool isNullable;
    PGDataType dataType;
    int index;
};

int main(int argc, char** argv)
{
    DatabaseInfo config;
    parseFileIntoConfig("test.json", config);
    std::cout << config.host << " - " << config.dbName << " - " << config.username << " - " << config.password << '\n';
    std::cout << "Params: \n";
    for(int i = 0; i < argc; i++) {
        std::cout << argv[i] <<  '\n';
    }
    std::cout << '\n';
    auto beforeTime = std::chrono::steady_clock::now();
    std::string retailerQuery = getForeignKeyQuery(argv[1]);
    try {
        pgfe::Connection conn{pgfe::Connection_options{}
            .set(pgfe::Communication_mode::net)
            .set_hostname("localhost")
            .set_database("deductions_app_development")
            .set_username("postgres")
            .set_password("postgres")
            .set_ssl_enabled(false)
        };
        conn.connect();

        pgfe::Connection local{pgfe::Connection_options{}
            .set(pgfe::Communication_mode::net)
            .set_hostname("localhost")
            .set_database("deductions_app_development")
            .set_username("postgres")
            .set_password("postgres")
            //.set_ssl_enabled(true)
        };

        local.connect();
        
        std::unordered_set<std::string> seen;
        std::map<std::string, std::unordered_set<std::string>> deps;
        std::map<std::string, std::unordered_set<std::string>> inv;
        std::unordered_map<std::string, std::unordered_set<std::string>> locks;
        std::unordered_map<std::string, std::unordered_map<std::string, std::string>> fkeys;
        std::unordered_map<std::string, std::unordered_map<std::string, std::string>> invFkeys;
        std::unordered_map<std::string, std::unordered_map<std::string, std::string>> fkeyCols;  // fkeyCols[table_foo][column_name] = foreign_column_name
        std::unordered_map<std::string, std::unordered_set<std::string>> tableFkeyNeeds;
        std::unordered_map<std::string, std::unordered_map<std::string, ColInfo>> tableCols;
        std::vector<std::string> tableOrder = {argv[1]};
        std::queue<std::string> q;
        q.push(std::string(argv[1]));
        seen.insert(std::string(argv[1]));
        while(!q.empty()) {
            std::string curr = q.front();
            q.pop();

            std::string query = getForeignKeyQuery(curr);
            std::string supporter = getSupporterQuery(curr);
            conn.execute([&](auto&& r)
            {
                using dmitigr::pgfe::to;
                std::vector<std::string> cols;
                auto foo = to<std::string>(r["tableName"]);
                auto colName = to<std::string>(r["column_name"]);
                auto foreignColName = to<std::string>(r["foreign_column_name"]);
                fkeyCols[curr][colName] = foreignColName;
                tableFkeyNeeds[curr].insert(foreignColName);
                    fkeys[foo][curr] = colName; // supporter's col name 
                    invFkeys[curr][foo] = colName;
                if(seen.count(foo) == 0) {
                    seen.insert(foo);
                    q.push(foo);
                    //std::cout << "Newly seen: " << foo << '\n';
                    //std::cout << "Column Name for FKEY: " << colName << '\n';
                }
                // deps[B] = [A..] B depends on
                // inv[A] = [B..] A supports 
                deps[foo].insert(curr);
                inv[curr].insert(foo);
                tableOrder.push_back(foo);
            },
            query);
            conn.execute([&](auto&& r){
                using dmitigr::pgfe::to;
                auto tableName = to<std::string>(r["foreign_table_name"]);
                std::cout << curr << " depends on: " << tableName << '\n';
                if(seen.count(tableName) == 0) {
                    seen.insert(tableName);
                    q.push(tableName);
                }
                deps[curr].insert(tableName);
                inv[tableName].insert(curr);
                
            }, supporter);
            std::string colQuery = getTableFieldsAndDataTypes(curr);
            //std::cout << colQuery << '\n';
            conn.execute([&](auto&& r) {
                using dmitigr::pgfe::to;
                std::string colName = to<std::string>(r["column_name"]);
                std::string isNullable = to<std::string>(r["is_nullable"]);
                std::string dataType = to<std::string>(r["data_type"]);
                tableCols[curr][colName].isNullable = isNullable == "YES" ? true : false;
                tableCols[curr][colName].dataType = getPGDataType(dataType);

                //std::cout << colName << " " << isNullable << " " << dataType  << '\n';
            }, colQuery);
        }
        auto depCopy = deps;
        std::vector<std::string> order;
        q.push(argv[1]);

        auto hasIncomingEdges =[&](const std::string& table) {
            return inv.count(table) > 0;
        };

        for(auto& d : deps) {
            std::cout << d.first << " depends on: ";
            for(auto& di: d.second) {
                std::cout << di << " | ";
            }
            std::cout << '\n';
        }

        // kahn's algorithm
        std::vector<std::string> L;
        std::queue<std::string> S;

        for(auto& table : seen) {
            if(deps[table].size() == 0) {
                std::cout << "Adding: " << table << '\n';
                S.push(table);
            }
        }   

        // https://en.wikipedia.org/wiki/Topological_sorting#:~:text=.-,Kahn%27s%20algorithm,-%5Bedit%5D
        while(!S.empty()) {
            std::string curr = S.front();
            std::cout << "S.front(): " << curr << '\n';
            S.pop();
            L.push_back(curr);
            std::vector<std::string> toErase;
            for(std::string m : inv[curr]) {
                toErase.push_back(m);
                if(deps[m].size() == 1) {
                    S.push(m);
                }  
            }
            for(auto& erase : toErase) {
                inv[curr].erase(erase);
                deps[erase].erase(curr);
            }
        }
        
        std::unordered_map<std::string, std::unordered_map<std::string, std::vector<std::string>>> tableColValues;

        conn.execute([&](auto&& r)
            {
                using dmitigr::pgfe::to;
                auto id = to<std::string>(r["id"]);
                for(auto& col : tableFkeyNeeds[argv[1]]) {
                    auto ye = to<std::string>(r[col]);
                    tableColValues[argv[1]][col].push_back(ye);
                }
            },
            ("select * from " + std::string(argv[1]) + " where id = " + std::string(argv[2])));

        auto whereCondition = [&](std::string tableName) {
            std::string whereCondition = "";
            std::cout << tableName << '\n';
            bool first = true;
            for(auto& dep : depCopy[tableName]) {
                std::string tableCol = fkeys[tableName][dep];
                std::string mappedTableCol = fkeyCols[dep][tableCol];
                std::vector<std::string> values = tableColValues[dep][fkeyCols[dep][fkeys[tableName][dep]]];
                std::string currentCondition = "";
                    if(first) {
                        currentCondition = "WHERE ";
                        first = false;
                    } else {
                        currentCondition = " AND ";
                    }
                    currentCondition += ("\"" + fkeys[tableName][dep]);
                    currentCondition += "\" IN (";

                if(values.size() > 0) {
                    currentCondition += valuesFromVector(values);
                } else currentCondition += "NULL";
                currentCondition += ")";
                whereCondition += currentCondition;
            }
            return whereCondition;
        };
        int64_t totalRows = 0;
        auto runTable = [&](const std::string& tableName) {
            std::string query = R"(
                SELECT
                    *
                FROM 
            )" + tableName
            + R"(
            )" +
            whereCondition(tableName);
            std::cout << query << "\n";
            std::string copyQuery = "COPY(" + query + ") TO '/tmp/" + tableName + ".csv' WITH DELIMITER ',' CSV";
            //std::cout << "copy Query: " << copyQuery << '\n';
            bool ran = false;
            bool first = true;
            std::vector<std::string> colNames;
            /*
            conn.execute([&](auto&& r)
            {   
                using dmitigr::pgfe::to;
                /*
                std::vector<std::string> values;
                 std::string insertQuery;
                if(!ran) {
                    ran = true;
                    for(size_t i = 0; i < r.field_count(); i++) {
                        std::string value = to<std::string>(r[i]);
                        std::string fieldName(r.field_name(i));
                        if(pgDataTypeNeedsEnclosedQuotes(tableCols[tableName][fieldName].dataType) && value != "") {
                            std::string enclose = "'";
                            if(tableCols[tableName][fieldName].dataType == PGDataType::CHARACTERVARYING) {
                            bool hasSingleQuote = false;
                                for(auto& chr : value) {
                                    if(chr == '\'') {
                                        hasSingleQuote = true;
                                        break;
                                    }
                                }
                                if(hasSingleQuote) enclose =" $$ ";
                            }
                            value = enclose + value + enclose;
                        } else if(tableCols[tableName][fieldName].dataType == PGDataType::BOOLEAN) {
                            value = value == "t" ? "TRUE" : "FALSE";
                        }
                        values.push_back(value != "" ? value : "NULL");
                        if(first) {
                            colNames.push_back(fieldName);
                        }
                    }
                    //std::cout << '\n';
                    insertQuery = "INSERT INTO " + tableName + " ( " + valuesFromVector(colNames) + " ) " + "VALUES ( " + valuesFromVector(values) + " ) ON CONFLICT DO NOTHING";
                    outfile << valuesFromVector(values) << '\n';
                    std::cout << insertQuery << '\n';
                    //local.execute([&](auto&& r) {
                    //}, insertQuery);
                    first = false;
                }
                totalRows++;
                for(auto& col : tableFkeyNeeds[tableName]) {
                    auto colVal = to<std::string>(r[col]);
                    tableColValues[tableName][col].push_back(colVal);
                }
            },
            copyQuery);
            */
        };

        std::vector<std::string> dataSearchOrder = {argv[1]};
        for(int i = L.size() - 1; i >= 0; i--) {
            if(L[i] == argv[1]) continue;
            dataSearchOrder.push_back(L[i]);
        }
        for(int i = 0; i < dataSearchOrder.size(); i++) {
            runTable(dataSearchOrder[i]);
        }
        std::cout << "<-------------------------------------------->\nORDER:\n";
        for(auto& l : L) {
        std::cout << l << '\n';
        }


        std::chrono::time_point afterTime = std::chrono::steady_clock::now();
        std::chrono::duration<float> elapsedTime = afterTime - beforeTime;
        std::cout << "Program ran in: " << elapsedTime << '\n';
        std::cout << "Total Number of Rows: " << totalRows << '\n';

    } catch (const pgfe::Server_exception& e) {
        std::cout << e.error().detail() << '\n';
        assert(e.error().condition() == pgfe::Server_errc::c42_syntax_error);
        std::printf("Error %s is handled as expected.\n", e.error().sqlstate());
} catch (const std::exception& e) {
    std::printf("Oops: %s\n", e.what());
    return 1;

    }
}
