/*
 * ZeroTier One - Network Virtualization Everywhere
 * Copyright (C) 2011-2018  ZeroTier, Inc.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifdef ZT_CONTROLLER_USE_LIBPQ

#include "PostgreSQL.hpp"
#include "EmbeddedNetworkController.hpp"
#include "../version.h"

#include <libpq-fe.h>

using json = nlohmann::json;
namespace {

static const char *_timestr()
{
	time_t t = time(0);
	char *ts = ctime(&t);
	char *p = ts;
	if (!p)
		return "";
	while (*p) {
		if (*p == '\n') {
			*p = (char)0;
			break;
		}
		++p;
	}
	return ts;
}

}

using namespace ZeroTier;

PostgreSQL::PostgreSQL(EmbeddedNetworkController *const nc, const Identity &myId, const char *path)
    : DB(nc, myId, path)
    , _ready(0)
	, _connected(1)
    , _run(1)
    , _waitNoticePrinted(false)
{
	_connString = std::string(path);

	_readyLock.lock();
	_heartbeatThread = std::thread(&PostgreSQL::heartbeat, this);
	_membersDbWatcher = std::thread(&PostgreSQL::membersDbWatcher, this);
	_networksDbWatcher = std::thread(&PostgreSQL::networksDbWatcher, this);
	for (int i = 0; i < ZT_CONTROLLER_RETHINKDB_COMMIT_THREADS; ++i) {
		_commitThread[i] = std::thread(&PostgreSQL::commitThread, this);
	}
	_onlineNotificationThread = std::thread(&PostgreSQL::onlineNotificationThread, this);
}

PostgreSQL::~PostgreSQL()
{
	_run = 0;
	std::this_thread::sleep_for(std::chrono::milliseconds(100));
	
	_heartbeatThread.join();
	_membersDbWatcher.join();
	_networksDbWatcher.join();
	for (int i = 0; i < ZT_CONTROLLER_RETHINKDB_COMMIT_THREADS; ++i) {
		_commitThread[i].join();
	}
	_onlineNotificationThread.join();

}


bool PostgreSQL::waitForReady()
{
	while (_ready < 2) {
		if (!_waitNoticePrinted) {
			_waitNoticePrinted = true;
			fprintf(stderr, "[%s] NOTICE: %.10llx controller PostgreSQL waiting for initial data download..." ZT_EOL_S, ::_timestr(), (unsigned long long)_myAddress.toInt());
		}
		_readyLock.lock();
		_readyLock.unlock();
	}
	return true;
}

bool PostgreSQL::isReady()
{
	return ((_ready == 2)&&(_connected));
}

void PostgreSQL::save(nlohmann::json *orig, nlohmann::json &record)
{
	if (!record.is_object()) {
		return;
	}
	waitForReady();
	if (orig) {
		if (*orig != record) {
			record["revision"] = OSUtils::jsonInt(record["revision"],0ULL) + 1;
			_commitQueue.post(new nlohmann::json(record));
		}
	} else {
		record["revision"] = 1;
		_commitQueue.post(new nlohmann::json(record));
	}
}

void PostgreSQL::eraseNetwork(const uint64_t networkId)
{
	char tmp2[24];
	waitForReady();
	Utils::hex(networkId, tmp2);
	json *tmp = new json();
	(*tmp)["id"] = tmp2;
	(*tmp)["objtype"] = "_delete_network";
	_commitQueue.post(tmp);
}

void PostgreSQL::eraseMember(const uint64_t networkId, const uint64_t memberId) 
{
	char tmp2[24];
	json *tmp = new json();
	Utils::hex(networkId, tmp2);
	(*tmp)["nwid"] = tmp2;
	Utils::hex(memberId, tmp2);
	(*tmp)["id"] = tmp2;
	(*tmp)["objtype"] = "_delete_member";
	_commitQueue.post(tmp);
}

void PostgreSQL::nodeIsOnline(const uint64_t networkId, const uint64_t memberId, const InetAddress &physicalAddress)
{
	std::lock_guard<std::mutex> l(_lastOnline_l);
	std::pair<int64_t, InetAddress> &i = _lastOnline[std::pair<uint64_t,uint64_t>(networkId, memberId)];
	i.first = OSUtils::now();
	if (physicalAddress) {
		i.second = physicalAddress;
	}
}

void PostgreSQL::initializeNetworks(PGconn *conn)
{
	try {
		if (PQstatus(conn) != CONNECTION_OK) {
			fprintf(stderr, "Bad Database Connection: %s", PQerrorMessage(conn));
			exit(1);
		}

		const char *params[1] = {
			_myAddressStr.c_str()
		};

		PGresult *res = PQexecParams(conn, "SELECT id, EXTRACT(EPOCH FROM creation_time AT TIME ZONE 'UTC')*1000, capabilities, "
			"enable_broadcast, EXTRACT(EPOCH FROM last_modified AT TIME ZONE 'UTC')*1000, mtu, multicast_limit, name, private, remote_trace_level, "
			"remote_trace_target, revision, rules, tags, v4_assign_mode, v6_assign_mode FROM ztc_network "
			"WHERE deleted = false AND controller_id = $1",
			1,
			NULL,
			params,
			NULL,
			NULL,
			0);
		
		if (PQresultStatus(res) != PGRES_TUPLES_OK) {
			fprintf(stderr, "Networks Initialization Failed: %s", PQerrorMessage(conn));
			PQclear(res);
			exit(1);
		}

		int numRows = PQntuples(res);
		for (int i = 0; i < numRows; ++i) {
			json empty;
			json config;

			config["id"] = PQgetvalue(res, i, 0);
			config["nwid"] = PQgetvalue(res, i, 0);
			config["creationTime"] = std::stoull(PQgetvalue(res, i, 1));
			config["capabilities"] = json::parse(PQgetvalue(res, i, 2));
			config["enableBroadcast"] = (strcmp(PQgetvalue(res, i, 3),"t")==0);
			config["lastModified"] = std::stoull(PQgetvalue(res, i, 4));
			config["mtu"] = std::stoi(PQgetvalue(res, i, 5));
			config["multicastLimit"] = std::stoi(PQgetvalue(res, i, 6));
			config["name"] = PQgetvalue(res, i, 7);
			config["private"] = (strcmp(PQgetvalue(res, i, 8),"t")==0);
			config["remoteTraceLevel"] = std::stoi(PQgetvalue(res, i, 9));
			config["remoteTraceTarget"] = PQgetvalue(res, i, 10);
			config["revision"] = std::stoull(PQgetvalue(res, i, 11));
			config["rules"] = json::parse(PQgetvalue(res, i, 12));
			config["tags"] = json::parse(PQgetvalue(res, i, 13));
			config["v4AssignMode"] = json::parse(PQgetvalue(res, i, 14));
			config["v6AssignMode"] = json::parse(PQgetvalue(res, i, 15));
			config["objtype"] = "network";
			config["ipAssignmentPools"] = json::array();
			config["routes"] = json::array();

			PGresult *r2 = PQexecParams(conn,
				"SELECT host(ip_range_start), host(ip_range_end) FROM ztc_network_assignment_pool WHERE network_id = $1",
				1,
				NULL,
				params,
				NULL,
				NULL,
				0);
			
			if (PQresultStatus(r2) != PGRES_TUPLES_OK) {
				fprintf(stderr, "ERROR: Error retreiving IP pools for network: %s\n", PQresultErrorMessage(r2));
				PQclear(r2);
				PQclear(res);
				exit(1);
			}

			int n = PQntuples(r2);
			for (int j = 0; j < n; ++j) {
				json ip;
				ip["ipRangeStart"] = PQgetvalue(r2, j, 0);
				ip["ipRangeEnd"] = PQgetvalue(r2, j, 1);

				config["ipAssignmentPools"].push_back(ip);
			}

			PQclear(r2);

			r2 = PQexecParams(conn,
				"SELECT host(address), bits, host(via) FROM ztc_network_route WHERE network_id = $1",
				1,
				NULL,
				params,
				NULL,
				NULL,
				0);

			if (PQresultStatus(r2) != PGRES_TUPLES_OK) {
				fprintf(stderr, "ERROR: Error retreiving routes for network: %s\n", PQresultErrorMessage(r2));
				PQclear(r2);
				PQclear(res);
				exit(1);
			}

			n = PQntuples(r2);
			for (int j = 0; j < n; ++j) {
				std::string addr = PQgetvalue(r2, j, 0);
				std::string bits = PQgetvalue(r2, j, 1);
				std::string via = PQgetvalue(r2, j, 2);
				fprintf(stderr, "via: %s", via.c_str());
				json route;
				route["target"] = addr + "/" + bits;

				if (via == "NULL") {
					route["via"] = nullptr;
				} else {
					route["via"] = via;
				}
				config["routes"].push_back(route);
			}

			PQclear(r2);
			
			_networkChanged(empty, config, false);
		}

		PQclear(res);

		if (++this->_ready == 2) {
			if (_waitNoticePrinted) {
				fprintf(stderr,"[%s] NOTICE: %.10llx controller PostgreSQL data download complete." ZT_EOL_S,_timestr(),(unsigned long long)_myAddress.toInt());
			}
			_readyLock.unlock();
		}
	} catch (std::exception &e) {
		fprintf(stderr, "ERROR: Error initializing networks: %s", e.what());
		exit(-1);
	}
}

void PostgreSQL::initializeMembers(PGconn *conn)
{
	try {
		if (PQstatus(conn) != CONNECTION_OK) {
			fprintf(stderr, "Bad Database Connection: %s", PQerrorMessage(conn));
			exit(1);
		}

		const char *params[1] = {
			_myAddressStr.c_str()
		};

		PGresult *res = PQexecParams(conn,
			"SELECT m.id, m.network_id, m.active_bridge, m.authorized, m.capabilities, EXTRACT(EPOCH FROM m.creation_time AT TIME ZONE 'UTC')*1000, m.identity, "
			"	EXTRACT(EPOCH FROM m.last_authorized_time AT TIME ZONE 'UTC')*1000, "
			"	EXTRACT(EPOCH FROM m.last_deauthorized_time AT TIME ZONE 'UTC')*1000, "
			"	m.remote_trace_level, m.remote_trace_target, m.tags, m.v_major, m.v_minor, m.v_rev, m.v_proto, "
			"	m.no_auto_assign_ips, m.revision "
			"FROM ztc_member m "
			"INNER JOIN ztc_network n "
			"	ON n.id = m.network_id "
			"WHERE n.controller_id = $1 AND m.deleted = false",
			1,
			NULL,
			params,
			NULL,
			NULL,
			0);

		if (PQresultStatus(res) != PGRES_TUPLES_OK) {
			fprintf(stderr, "Member Initialization Failed: %s", PQerrorMessage(conn));
			PQclear(res);
			exit(1);
		}

		int numRows = PQntuples(res);
		for (int i = 0; i < numRows; ++i) {
			json empty;
			json config;

			std::string memberId(PQgetvalue(res, i, 0));
			std::string networkId(PQgetvalue(res, i, 1));
			std::string ctime = PQgetvalue(res, i, 5);
			config["id"] = memberId;
			config["nwid"] = networkId;
			config["activeBridge"] = (strcmp(PQgetvalue(res, i, 2), "t") == 0);
			config["authorized"] = (strcmp(PQgetvalue(res, i, 3), "t") == 0);
			config["capabilities"] = json::parse(PQgetvalue(res, i, 4));
			config["creationTime"] = std::stoull(PQgetvalue(res, i, 5));
			config["identity"] = PQgetvalue(res, i, 6);
			config["lastAuthorizedTime"] = std::stoull(PQgetvalue(res, i, 7));
			config["lastDeauthorizedTime"] = std::stoull(PQgetvalue(res, i, 8));
			config["remoteTraceLevel"] = std::stoi(PQgetvalue(res, i, 9));
			config["remoteTraceTarget"] = PQgetvalue(res, i, 10);
			config["tags"] = json::parse(PQgetvalue(res, i, 11));
			config["vMajor"] = std::stoi(PQgetvalue(res, i, 12));
			config["vMinor"] = std::stoi(PQgetvalue(res, i, 13));
			config["vRev"] = std::stoi(PQgetvalue(res, i, 14));
			config["vProto"] = std::stoi(PQgetvalue(res, i, 15));
			config["noAutoAssignIps"] = (strcmp(PQgetvalue(res, i, 16), "t") == 0);
			config["revision"] = std::stoull(PQgetvalue(res, i, 17));
			config["objtype"] = "member";
			config["ipAssignments"] = json::array();
			const char *p2[2] = {
				memberId.c_str(),
				networkId.c_str()
			};

			PGresult *r2 = PQexecParams(conn,
				"SELECT address FROM ztc_member_ip_assignment WHERE member_id = $1 AND network_id = $2",
				2,
				NULL,
				p2,
				NULL,
				NULL,
				0);

			if (PQresultStatus(r2) != PGRES_TUPLES_OK) {
				fprintf(stderr, "Member Initialization Failed: %s", PQerrorMessage(conn));
				PQclear(r2);
				PQclear(res);
				exit(1);
			}

			int n = PQntuples(r2);
			for (int j = 0; j < n; ++j) {
				config["ipAssignments"].push_back(PQgetvalue(r2, j, 0));
			}

			_memberChanged(empty, config, false);
		}

		PQclear(res);

		if (++this->_ready == 2) {
			if (_waitNoticePrinted) {
				fprintf(stderr,"[%s] NOTICE: %.10llx controller PostgreSQL data download complete." ZT_EOL_S,_timestr(),(unsigned long long)_myAddress.toInt());
			}
			_readyLock.unlock();
		}
	} catch (std::exception &e) {
		fprintf(stderr, "ERROR: Error initializing members: %s\n", e.what());
		exit(-1);
	}
}

void PostgreSQL::heartbeat()
{
	char publicId[1024];
	char hostnameTmp[1024];
	_myId.toString(false,publicId);
	if (gethostname(hostnameTmp, sizeof(hostnameTmp))!= 0) {
		hostnameTmp[0] = (char)0;
	} else {
		for (int i = 0; i < sizeof(hostnameTmp); ++i) {
			if ((hostnameTmp[i] == '.')||(hostnameTmp[i] == 0)) {
				hostnameTmp[i] = (char)0;
				break;
			}
		}
	}
	const char *controllerId = _myAddressStr.c_str();
	const char *publicIdentity = publicId;
	const char *hostname = hostnameTmp;

	PGconn *conn = PQconnectdb(_path.c_str());
	if (PQstatus(conn) == CONNECTION_BAD) {
		fprintf(stderr, "Connection to database failed: %s\n", PQerrorMessage(conn));
		PQfinish(conn);
		exit(1);
	}
	while (_run == 1) {
		if(PQstatus(conn) != CONNECTION_OK) {
			PQfinish(conn);
			conn = PQconnectdb(_path.c_str());
		}
		if (conn) {
			const char *values[8] = {
				controllerId,
				hostname,
				std::to_string(OSUtils::now()).c_str(),
				publicIdentity,
				std::to_string(ZEROTIER_ONE_VERSION_MAJOR).c_str(),
				std::to_string(ZEROTIER_ONE_VERSION_MINOR).c_str(),
				std::to_string(ZEROTIER_ONE_VERSION_REVISION).c_str(),
				std::to_string(ZEROTIER_ONE_VERSION_BUILD).c_str()
			};
			int lengths[8] = {
				(int)strlen(values[0]),
				(int)strlen(values[1]),
				(int)strlen(values[2]),
				(int)strlen(values[3]),
				(int)strlen(values[4]),
				(int)strlen(values[5]),
				(int)strlen(values[6]),
				(int)strlen(values[7])
			};
			int binary[8] = {0,0,0,0,0,0,0,0};

			PGresult *res = PQexecParams(conn,
				"INSERT INTO ztc_controller (id, cluster_host, last_alive, public_identity, v_major, v_minor, v_rev, v_build) " 
				"VALUES ($1, $2, TO_TIMESTAMP($3::double precision/1000), $4, $5, $6, $7, $8) "
				"ON CONFLICT (id) DO UPDATE SET cluster_host = EXCLUDED.cluster_host, last_alive = EXCLUDED.last_alive, "
				"public_identity = EXCLUDED.public_identity, v_major = EXCLUDED.v_major, v_minor = EXCLUDED.v_minor, "
				"v_rev = EXCLUDED.v_rev, v_build = EXCLUDED.v_rev",
				8,       // number of parameters
				NULL,    // oid field.   ignore
				values,  // values for substitution
				lengths, // lengths in bytes of each value
				binary,  // binary?
				0);

			if (PQresultStatus(res) != PGRES_COMMAND_OK) {
				fprintf(stderr, "Heartbeat Update Failed: %s\n", PQresultErrorMessage(res));
			}
			PQclear(res);
		}

		std::this_thread::sleep_for(std::chrono::milliseconds(1000));
	}

	PQfinish(conn);
	conn = NULL;
}

void PostgreSQL::membersDbWatcher()
{
	PGconn *conn = PQconnectdb(_path.c_str());
	if (PQstatus(conn) == CONNECTION_BAD) {
		fprintf(stderr, "Connection to database failed: %s\n", PQerrorMessage(conn));
		PQfinish(conn);
		exit(1);
	}

	initializeMembers(conn);

	char buf[11] = {0};
	std::string cmd = "LISTEN member_" + std::string(_myAddress.toString(buf));
	PGresult *res = PQexec(conn, cmd.c_str());
	if (!res || PQresultStatus(res) != PGRES_COMMAND_OK) {
		fprintf(stderr, "LISTEN command failed: %s\n", PQresultErrorMessage(res));
		PQclear(res);
		PQfinish(conn);
		exit(1);
	}

	PQclear(res); res = NULL;

	while(_run == 1) {
		if (PQstatus(conn) != CONNECTION_OK) {
			fprintf(stderr, "ERROR: Member Watcher lost connection to Postgres.");
			exit(-1);
		}
		PGnotify *notify = NULL;
		PQconsumeInput(conn);
		while ((notify = PQnotifies(conn)) != NULL) {
			fprintf(stderr, "ASYNC NOTIFY of '%s' id:%s received\n", notify->relname, notify->extra);

			try {
				json tmp(json::parse(notify->extra));
				json &ov = tmp["old_val"];
				json &nv = tmp["new_val"];
				json oldConfig, newConfig;
				if (ov.is_object()) oldConfig = ov;
				if (nv.is_object()) newConfig = nv;
				if (oldConfig.is_object() || newConfig.is_object()) {
					_memberChanged(oldConfig,newConfig,(this->_ready>=2));
				}
			} catch (...) {} // ignore bad records

			free(notify);
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	PQfinish(conn);
	conn = NULL;
}

void PostgreSQL::networksDbWatcher()
{
	PGconn *conn = PQconnectdb(_path.c_str());
	if (PQstatus(conn) == CONNECTION_BAD) {
		fprintf(stderr, "Connection to database failed: %s\n", PQerrorMessage(conn));
		PQfinish(conn);
		exit(1);
	}

	initializeNetworks(conn);

	char buf[11] = {0};
	std::string cmd = "LISTEN network_" + std::string(_myAddress.toString(buf));
	PGresult *res = PQexec(conn, cmd.c_str());
	if (!res || PQresultStatus(res) != PGRES_COMMAND_OK) {
		fprintf(stderr, "LISTEN command failed: %s\n", PQresultErrorMessage(res));
		PQclear(res);
		PQfinish(conn);
		exit(1);
	}

	PQclear(res); res = NULL;

	while(_run == 1) {
		if (PQstatus(conn) != CONNECTION_OK) {
			fprintf(stderr, "ERROR: Network Watcher lost connection to Postgres.");
			exit(-1);
		}
		PGnotify *notify = NULL;
		PQconsumeInput(conn);
		while ((notify = PQnotifies(conn)) != NULL) {
			fprintf(stderr, "ASYNC NOTIFY of '%s' id:%s received\n", notify->relname, notify->extra);
			try {
				json tmp(json::parse(notify->extra));
				json &ov = tmp["old_val"];
				json &nv = tmp["new_val"];
				json oldConfig, newConfig;
				if (ov.is_object()) oldConfig = ov;
				if (nv.is_object()) newConfig = nv;
				if (oldConfig.is_object()||newConfig.is_object()) {
					_networkChanged(oldConfig,newConfig,(this->_ready >= 2));
				}
			} catch (...) {} // ignore bad records
			free(notify);
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	PQfinish(conn);
	conn = NULL;
}

void PostgreSQL::commitThread()
{
	PGconn *conn = PQconnectdb(_path.c_str());
	if (PQstatus(conn) == CONNECTION_BAD) {
		fprintf(stderr, "ERROR: Connection to database failed: %s\n", PQerrorMessage(conn));
		PQfinish(conn);
		exit(1);
	}

	json *config = nullptr;
	while(_commitQueue.get(config)&(_run == 1)) {
		if (!config) {
			continue;
		}
		if (PQstatus(conn) == CONNECTION_BAD) {
			fprintf(stderr, "ERROR: Connection to database failed: %s\n", PQerrorMessage(conn));
			PQfinish(conn);
			exit(1);
		}
		try { 
			const std::string objtype = (*config)["objtype"];
			if (objtype == "member") {
				try {
					std::string memberId = (*config)["id"];
					std::string networkId = (*config)["nwid"];
					std::string identity = (*config)["identity"];
					std::string target = "NULL";
					if (!(*config)["remoteTraceTarget"].is_null()) {
						target = (*config)["remoteTraceTarget"];
					}
					const char *values[19] = {
						memberId.c_str(),
						networkId.c_str(),
						((*config)["activeBridge"] ? "true" : "false"),
						((*config)["authorized"] ? "true" : "false"),
						OSUtils::jsonDump((*config)["capabilities"], -1).c_str(),
						identity.c_str(),
						std::to_string((long long)(*config)["lastAuthorizedTime"]).c_str(),
						std::to_string((long long)(*config)["lastDeauthorizedTime"]).c_str(),
						((*config)["noAutoAssignIps"] ? "true" : "false"),
						std::to_string((int)(*config)["remoteTraceLevel"]).c_str(),
						(target == "NULL") ? NULL : target.c_str(),
						std::to_string((unsigned long long)(*config)["revision"]).c_str(),
						OSUtils::jsonDump((*config)["tags"], -1).c_str(),
						std::to_string((int)(*config)["vMajor"]).c_str(),
						std::to_string((int)(*config)["vMinor"]).c_str(),
						std::to_string((int)(*config)["vRev"]).c_str(),
						std::to_string((int)(*config)["vProto"]).c_str()
					};

					PGresult *res = PQexecParams(conn,
						"INSERT INTO ztc_member (id, network_id, active_bridge, authorized, capabilities, "
						"identity, last_authorized_time, last_deauthorized_time, no_auto_assign_ips, "
						"remote_trace_level, remote_trace_target, revision, tags, v_major, v_minor, v_rev, v_proto) "
						"VALUES ($1, $2, $3, $4, $5, $6, "
						"TO_TIMESTAMP($7::double precision/1000), TO_TIMESTAMP($8::double precision/1000), "
						"$9, $10, $11, $12, $13, $14, $15, $16, $17) ON CONFLICT (network_id, id) DO UPDATE SET "
						"active_bridge = EXCLUDED.active_bridge, authorized = EXCLUDED.authorized, capabilities = EXCLUDED.capabilities, "
						"identity = EXCLUDED.identity, last_authorized_time = EXCLUDED.last_authorized_time, "
						"last_deauthorized_time = EXCLUDED.last_deauthorized_time, no_auto_assign_ips = EXCLUDED.no_auto_assign_ips, "
						"remote_trace_level = EXCLUDED.remote_trace_level, remote_trace_target = EXCLUDED.remote_trace_target, "
						"revision = EXCLUDED.revision+1, tags = EXCLUDED.tags, v_major = EXCLUDED.v_major, "
						"v_minor = EXCLUDED.v_minor, v_rev = EXCLUDED.v_rev, v_proto = EXCLUDED.v_proto",
						17,
						NULL,
						values,
						NULL,
						NULL,
						0);
					
					if (PQresultStatus(res) != PGRES_COMMAND_OK) {
						fprintf(stderr, "ERROR: Error updating member: %s\n", PQresultErrorMessage(res));
						fprintf(stderr, "%s", OSUtils::jsonDump(*config, 2).c_str());
						PQclear(res);
						continue;
					}

					PQclear(res);

					res = PQexec(conn, "BEGIN");
					if (PQresultStatus(res) != PGRES_COMMAND_OK) {
						fprintf(stderr, "ERROR: Error beginning transaction: %s\n", PQresultErrorMessage(res));
						PQclear(res);
						continue;
					}

					PQclear(res);

					const char *v2[2] = {
						memberId.c_str(),
						networkId.c_str()
					};

					res = PQexecParams(conn,
						"DELETE FROM ztc_member_ip_assignment WHERE member_id = $1 AND network_id = $2",
						2,
						NULL,
						v2,
						NULL,
						NULL,
						0);

					if (PQresultStatus(res) != PGRES_COMMAND_OK) {
						fprintf(stderr, "ERROR: Error updating IP address assignments: %s\n", PQresultErrorMessage(res));
						PQclear(res);
						PQclear(PQexec(conn, "ROLLBACK"));;
						continue;
					}

					PQclear(res);

					for (auto i = (*config)["ipAssignments"].begin(); i != (*config)["ipAssignments"].end(); ++i) {
						std::string addr = *i;
						const char *v3[3] = {
							memberId.c_str(),
							networkId.c_str(),
							addr.c_str()
						};

						res = PQexecParams(conn,
							"INSERT INTO ztc_member_ip_assignment (member_id, network_id, address) VALUES ($1, $2, $3)",
							3,
							NULL,
							v3,
							NULL,
							NULL,
							0);
						
						if (PQresultStatus(res) != PGRES_COMMAND_OK) {
							fprintf(stderr, "ERROR: Error setting IP addresses for member: %s\n", PQresultErrorMessage(res));
							PQclear(res);
							PQclear(PQexec(conn, "ROLLBACK"));
							continue;
						}
					}

					res = PQexec(conn, "COMMIT");
					if (PQresultStatus(res) != PGRES_COMMAND_OK) {
						fprintf(stderr, "ERROR: Error committing ip address data: %s\n", PQresultErrorMessage(res));
					}

					PQclear(res);
				} catch (std::exception &e) {
					fprintf(stderr, "ERROR: Error updating member: %s\n", e.what());
				}
			} else if (objtype == "network") {
				try {
					std::string id = (*config)["id"];
					std::string controllerId = _myAddressStr.c_str();
					std::string name = (*config)["name"];
					std::string remoteTraceTarget("NULL");
					if (!(*config)["remoteTraceTarget"].is_null()) {
						remoteTraceTarget = (*config)["remoteTraceTarget"];
					}
					std::string rulesSource = (*config)["rulesSource"];
					const char *values[16] = {
						id.c_str(),
						controllerId.c_str(),
						OSUtils::jsonDump((*config)["capabilitles"], -1).c_str(),
						((*config)["enableBroadcast"] ? "true" : "false"),
						std::to_string(OSUtils::now()).c_str(),
						std::to_string((int)(*config)["mtu"]).c_str(),
						std::to_string((int)(*config)["multicastLimit"]).c_str(),
						name.c_str(),
						((*config)["private"] ? "true" : "false"),
						std::to_string((int)(*config)["remoteTraceLevel"]).c_str(),
						(remoteTraceTarget == "NULL" ? NULL : remoteTraceTarget.c_str()),
						OSUtils::jsonDump((*config)["rules"], -1).c_str(),
						rulesSource.c_str(),
						OSUtils::jsonDump((*config)["tags"], -1).c_str(),
						OSUtils::jsonDump((*config)["v4AssignMode"],-1).c_str(),
						OSUtils::jsonDump((*config)["v6AssignMode"], -1).c_str(),
					};

					PGresult *res = PQexecParams(conn,
						"UPDATE ztc_network SET controller_id = $2, capabilities = $3, enable_broadcast = $4, "
						"last_updated = $5, mtu = $6, multicast_limit = $7, name = $8, private = $9, "
						"remote_trace_level = $10, remote_trace_target = $11, rules = $12, rules_source = $13, "
						"tags = $14, v4_assign_mode = $15, v6_assign_mode = $16 "
						"WHERE id = $1",
						16,
						NULL,
						values,
						NULL,
						NULL,
						0);
					
					if (PQresultStatus(res) != PGRES_COMMAND_OK) {
						fprintf(stderr, "ERROR: Error updating network record: %s\n", PQresultErrorMessage(res));
						PQclear(res);
						continue;
					}

					PQclear(res);

					res = PQexec(conn, "BEGIN");
					if (PQresultStatus(res) != PGRES_COMMAND_OK) {
						fprintf(stderr, "ERROR: Error beginnning transaction: %s\n", PQresultErrorMessage(res));
						PQclear(res);
						continue;
					}

					PQclear(res);

					const char *params[1] = {
						id.c_str()
					};
					res = PQexecParams(conn, 
						"DELETE FROM ztc_network_assignment_pool WHERE network_id = $1",
						1,
						NULL,
						params,
						NULL,
						NULL,
						0);
					if (PQresultStatus(res) != PGRES_COMMAND_OK) {
						fprintf(stderr, "ERROR: Error updating assignment pool: %s\n", PQresultErrorMessage(res));
						PQclear(res);
						PQclear(PQexec(conn, "ROLLBACK"));
						continue;
					}

					PQclear(res);

					auto pool = (*config)["ipAssignmentPools"];
					bool err = false;
					for (auto i = pool.begin(); i != pool.end(); ++i) {
						std::string start = (*i)["ipRangeStart"];
						std::string end = (*i)["ipRangeEnd"];
						const char *p[3] = {
							id.c_str(),
							start.c_str(),
							end.c_str()
						};

						res = PQexecParams(conn,
							"INSERT INTO ztc_network_assignment_pool (network_id, ip_range_start, ip_range_end) "
							"VALUES ($1, $2, $3)",
							3,
							NULL,
							p,
							NULL,
							NULL,
							0);
						if (PQresultStatus(res) != PGRES_COMMAND_OK) {
							fprintf(stderr, "ERROR: Error updating assignment pool: %s\n", PQresultErrorMessage(res));
							PQclear(res);
							err = true;
							break;
						}
						PQclear(res);
					}
					if (err) {
						PQclear(PQexec(conn, "ROLLBACK"));
						continue;
					}

					res = PQexecParams(conn, 
						"DELETE FROM ztc_network_route WHERE network_id = $1",
						1,
						NULL,
						params,
						NULL,
						NULL,
						0);

					if (PQresultStatus(res) != PGRES_COMMAND_OK) {
						fprintf(stderr, "ERROR: Error updating routes: %s\n", PQresultErrorMessage(res));
						PQclear(res);
						PQclear(PQexec(conn, "ROLLBACK"));
						continue;
					}


					auto routes = (*config)["routes"];
					err = false;
					for (auto i = routes.begin(); i != routes.end(); ++i) {
						std::string t = (*i)["target"];
						std::vector<std::string> target;
						std::istringstream f(t);
						std::string s;
						while(std::getline(f, s, '/')) {
							target.push_back(s);
						}
						if (target.empty() || target.size() != 2) {
							continue;
						}
						std::string targetAddr = target[0];
						std::string targetBits = target[1];
						std::string via = "NULL";
						if (!(*i)["via"].is_null()) {
							via = (*i)["via"];
						}

						const char *p[4] = {
							id.c_str(),
							targetAddr.c_str(),
							targetBits.c_str(),
							(via == "NULL" ? NULL : via.c_str()),
						};

						res = PQexecParams(conn,
							"INSERT INTO ztc_network_route (network_id, address, bits, via) VALUES ($1, $2, $3, $4)",
							4,
							NULL,
							p,
							NULL,
							NULL,
							0);

						if (PQresultStatus(res) != PGRES_COMMAND_OK) {
							fprintf(stderr, "ERROR: Error updating routes: %s\n", PQresultErrorMessage(res));
							PQclear(res);
							err = true;
							break;
						}
						PQclear(res);
					}
					if (err) {
						PQclear(PQexec(conn, "ROLLBAcK"));
						continue;
					}

					res = PQexec(conn, "COMMIT");
					if (PQresultStatus(res) != PGRES_COMMAND_OK) {
						fprintf(stderr, "ERROR: Error committing network update: %s\n", PQresultErrorMessage(res));
					}
					PQclear(res);
				} catch (std::exception &e) {
					fprintf(stderr, "ERROR: Error updating member: %s\n", e.what());
				}
			} else if (objtype == "trace") {
				fprintf(stderr, "ERROR: Trace not yet implemented");
			} else if (objtype == "_delete_network") {
				try {
					std::string networkId = (*config)["nwid"];
					const char *values[1] = {
						networkId.c_str()
					};
					PGresult * res = PQexecParams(conn,
						"UPDATE ztc_network SET deleted = true WHERE id = $1",
						1,
						NULL,
						values,
						NULL,
						NULL,
						0);
					
					if (PQresultStatus(res) != PGRES_COMMAND_OK) {
						fprintf(stderr, "ERROR: Error deleting network: %s\n", PQresultErrorMessage(res));
					}

					PQclear(res);
				} catch (std::exception &e) {
					fprintf(stderr, "ERROR: Error deleting network: %s\n", e.what());
				}
			} else if (objtype == "_delete_member") {
				try {
					std::string memberId = (*config)["id"];
					std::string networkId = (*config)["nwid"];

					const char *values[2] = {
						memberId.c_str(),
						networkId.c_str()
					};

					PGresult *res = PQexecParams(conn,
						"UPDATE ztc_member SET hidden = true, deleted = true WHERE id = $1 AND network_id = $2",
						2,
						NULL,
						values,
						NULL,
						NULL,
						0);

					if (PQresultStatus(res) != PGRES_COMMAND_OK) {
						fprintf(stderr, "ERROR: Error deleting member: %s\n", PQresultErrorMessage(res));
					}

					PQclear(res);
				} catch (std::exception &e) {
					fprintf(stderr, "ERROR: Error deleting member: %s\n", e.what());
				}
			} else {
				fprintf(stderr, "ERROR: unknown objtype");
			}
		} catch (std::exception &e) {
			fprintf(stderr, "ERROR: Error getting objtype: %s\n", e.what());
		}
		delete config;

		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}

	PQfinish(conn);
}

void PostgreSQL::onlineNotificationThread()
{
	PGconn *conn = PQconnectdb(_path.c_str());
	if (PQstatus(conn) == CONNECTION_BAD) {
		fprintf(stderr, "Connection to database failed: %s\n", PQerrorMessage(conn));
		PQfinish(conn);
		exit(1);
	}
	_connected = 1;

	int64_t	lastUpdatedNetworkStatus = 0;
	std::unordered_map< std::pair<uint64_t,uint64_t>,int64_t,_PairHasher > lastOnlineCumulative;
	while (_run == 1) {
		if (PQstatus(conn) != CONNECTION_OK) {
			fprintf(stderr, "ERROR: Online Notification thread lost connection to Postgres.");
			exit(-1);
		}

		std::unordered_map< std::pair<uint64_t,uint64_t>,std::pair<int64_t,InetAddress>,_PairHasher > lastOnline;
		{
			std::lock_guard<std::mutex> l(_lastOnline_l);
			lastOnline.swap(_lastOnline);
		}

		PGresult *res = NULL;
		int qCount = 0;

		res = PQexec(conn, "BEGIN");
		if (PQresultStatus(res) != PGRES_COMMAND_OK) {
			fprintf(stderr, "ERROR: Error on BEGIN command (onlineNotificationThread): %s\n", PQresultErrorMessage(res));
			PQclear(res);
			exit(1);
		}
		PQclear(res);

		for (auto i=lastOnline.begin(); i != lastOnline.end(); ++i) {
			uint64_t nwid_i = i->first.first;
			char nwidTmp[64];
			char memTmp[64];
			char ipTmp[64];
			OSUtils::ztsnprintf(nwidTmp,sizeof(nwidTmp), "%.16llx", nwid_i);
			OSUtils::ztsnprintf(memTmp,sizeof(memTmp), "%.10llx", i->first.second);

			auto found = _networks.find(nwid_i);
			if (found == _networks.end()) {
				continue; // skip members trying to join non-existant networks
			}

			lastOnlineCumulative[i->first] = i->second.first;
			

			std::string networkId(nwidTmp);
			std::string memberId(memTmp);
			int64_t ts = i->second.first;
			std::string ipAddr = i->second.second.toIpString(ipTmp);

			const char *values[4] = {
				networkId.c_str(),
				memberId.c_str(),
				(ipAddr.empty() ? NULL : ipAddr.c_str()),
				std::to_string(ts).c_str(),
			};

			res = PQexecParams(conn,
				"INSERT INTO ztc_member_status (network_id, member_id, address, last_updated) VALUES ($1, $2, $3, TO_TIMESTAMP($4::double precision/1000)) "
				"ON CONFLICT (network_id, member_id) DO UPDATE SET address = EXCLUDED.address, last_updated = EXCLUDED.last_updated",
				4,       // number of parameters
				NULL,    // oid field.   ignore
				values,  // values for substitution
				NULL, // lengths in bytes of each value
				NULL,
				0);

			if (PQresultStatus(res) != PGRES_COMMAND_OK) {
				fprintf(stderr, "Error on Member Status upsert: %s\n", PQresultErrorMessage(res));
				PQclear(res);
				PQclear(PQexec(conn, "ROLLBACK"));
				continue;
			}

			PQclear(res);

			if ((++qCount) == 1024) {
				res = PQexec(conn, "COMMIT");
				if (PQresultStatus(res) != PGRES_COMMAND_OK) {
					fprintf(stderr, "ERROR: Error on commit (onlineNotificationThread): %s\n", PQresultErrorMessage(res));
					PQclear(res);
					PQexec(conn, "ROLLBACK");
					exit(1);
				}
				PQclear(res);

				res = PQexec(conn, "BEGIN");
				if (PQresultStatus(res) != PGRES_COMMAND_OK) {
					fprintf(stderr, "ERROR: Error on BEGIN (onlineNotificationThread): %s\n", PQresultErrorMessage(res));
					PQclear(res);
					exit(1);
				}
				PQclear(res);
				qCount = 0;
			}
		}
		res = PQexec(conn, "COMMIT");
		if (PQresultStatus(res) != PGRES_COMMAND_OK) {
			fprintf(stderr, "ERROR: Error on commit (onlineNotificationThread): %s\n", PQresultErrorMessage(res));
			PQclear(res);
			PQexec(conn, "ROLLBACK");
			exit(1);
		}
		PQclear(res);

		const int64_t now = OSUtils::now();
		if ((now - lastUpdatedNetworkStatus) > 10000) {
			lastUpdatedNetworkStatus = now;

			std::vector<std::pair<uint64_t, std::shared_ptr<_Network>>> networks;
			{
				std::lock_guard<std::mutex> l(_networks_l);
				for (auto i = _networks.begin(); i != _networks.end(); ++i) {
					networks.push_back(*i);
				}
			}

			int nCount = 0;

			res = PQexec(conn, "BEGIN");
			if (PQresultStatus(res) != PGRES_COMMAND_OK) {
				fprintf(stderr, "ERROR: Error on BEGIN command (onlineNotificationThread): %s\n", PQresultErrorMessage(res));
				PQclear(res);
				exit(1);
			}
			PQclear(res);
			for (auto i = networks.begin(); i != networks.end(); ++i) {
				char tmp[64];
				Utils::hex(i->first, tmp);

				std::string networkId(tmp);
				uint64_t authMemberCount = 0;
				uint64_t totalMemberCount = 0;
				uint64_t onlineMemberCount = 0;
				uint64_t bridgeCount = 0;
				uint64_t ts = now;
				{
					std::lock_guard<std::mutex> l2(i->second->lock);
					authMemberCount = i->second->authorizedMembers.size();
					totalMemberCount = i->second->members.size();
					bridgeCount = i->second->activeBridgeMembers.size();
					for (auto m=i->second->members.begin(); m != i->second->members.end(); ++m) {
						auto lo = lastOnlineCumulative.find(std::pair<uint64_t,uint64_t>(i->first, m->first));
						if (lo != lastOnlineCumulative.end()) {
							if ((now - lo->second) <= (ZT_NETWORK_AUTOCONF_DELAY * 2)) {
								++onlineMemberCount;
							} else {
								lastOnlineCumulative.erase(lo);
							}
						}
					}
				}

				const char *values[6] = {
					networkId.c_str(),
					std::to_string(bridgeCount).c_str(),
					std::to_string(authMemberCount).c_str(),
					std::to_string(onlineMemberCount).c_str(),
					std::to_string(totalMemberCount).c_str(),
					std::to_string(ts).c_str()
				};

				res = PQexecParams(conn, "INSERT INTO ztc_network_status (network_id, bridge_count, authorized_member_count, "
					"online_member_count, total_member_count, last_modified) VALUES ($1, $2, $3, $4, $5, TO_TIMESTAMP($6::double precision/1000)) "
					"ON CONFLICT (network_id) DO UPDATE SET bridge_count = EXCLUDED.bridge_count, "
					"authorized_member_count = EXCLUDED.authorized_member_count, online_member_count = EXCLUDED.online_member_count, "
					"total_member_count = EXCLUDED.total_member_count, last_modified = EXCLUDED.last_modified",
					6,
					NULL,
					values,
					NULL,
					NULL,
					0);
				
				if (PQresultStatus(res) != PGRES_COMMAND_OK) {
					fprintf(stderr, "ERROR: Error on Network Status upsert (onlineNotificationThread): %s\n", PQresultErrorMessage(res));
					PQclear(res);
					PQexec(conn, "ROLLBACK");
					exit(1);
				}

				if ((++nCount) == 1024) {
					res = PQexec(conn, "COMMIT");
					if (PQresultStatus(res) != PGRES_COMMAND_OK) {
						fprintf(stderr, "ERROR: Error on COMMIT (onlineNotificationThread): %s\n" , PQresultErrorMessage(res));
						PQclear(res);
						PQexec(conn, "ROLLBACK");
						exit(1);
					}

					res = PQexec(conn, "BEGIN");
					if (PQresultStatus(res) != PGRES_COMMAND_OK) {
						fprintf(stderr, "ERROR: Error on BEGIN command (onlineNotificationThread): %s\n", PQresultErrorMessage(res));
						PQclear(res);
						exit(1);
					}

					nCount = 0;
				}
			}
			res = PQexec(conn, "COMMIT");
			if (PQresultStatus(res) != PGRES_COMMAND_OK) {
				fprintf(stderr, "ERROR: Error on COMMIT (onlineNotificationThread): %s\n", PQresultErrorMessage(res));
				PQclear(res);
				PQexec(conn, "ROLLBACK");
				exit(1);
			}
		}

		std::this_thread::sleep_for(std::chrono::milliseconds(250));
	}
	PQfinish(conn);
}
#endif //ZT_CONTROLLER_USE_LIBPQ