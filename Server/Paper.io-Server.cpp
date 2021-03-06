﻿// Paper.io-Server.cpp: 定义控制台应用程序的入口点。
//

#include "stdafx.h"

using json = nlohmann::json;
using WsServer = SimpleWeb::SocketServer<SimpleWeb::WS>;
typedef shared_ptr<WsServer::Connection> Conn_Handle;

using namespace std;

/******************** Logging *****************/
#define log(...) printf(__VA_ARGS__), putchar('\n'), fflush(stdout)
/******************** Preferences ********************/
/** Server settings **/
const char SERVER_LISTEN[] = "0.0.0.0";
const char SERVER_ENDPOINT[] = "^/paper/?$";
const int SERVER_PORT = 8080;
const int SERVER_TIMEOUT = 5;
const int MAX_MSG_LENGTH = 256;

/** Validation settings **/
const int MAX_NAME_LEN = 12;

/** Skin settings **/
const char NO_SKIN[] = "000";

const char *SKIN_ALLOWED[] = {
	"000", "101", "102", "103", "104", "105", "106", "107", "108", "109", "110", "111"
};
const int SKIN_ALLOWED_SIZE = sizeof(SKIN_ALLOWED) / sizeof(char*);

const char *SKIN_RAND[] = {
	"001", "002", "003", "004", "005", "006", "007", "008", "009", "010", "011", "012"
};
const int SKIN_RAND_SIZE = sizeof( SKIN_RAND ) / sizeof( char* );

/** Map settings **/
const int MAX_W = 90;
const int MAX_H = 70;

const int BORDER_DIST = 10; //spawn dist to border >= BORDER_DIST
/** !!!Warning: PLAYER_DIST <= BORDER_DIST **/
const int PLAYER_DIST = min(BORDER_DIST, 5);

/** Move timer **/
const int MOVE_TIMER = 140;

/** ID Range **/
const int MIN_ID = 1000;
const int MAX_ID = 1000000000;

/** Room settings **/
const int MAX_ROOM = 20;
const int MIN_PLAYER = 2;
/** !!!Warning: MAX_PLAYER <= SKIN_RAND_SIZE **/
const int MAX_PLAYER = min(6, SKIN_RAND_SIZE);

/*** dx & dy ***/
const int dirKeyMap[4] = { 2, 3, 1, 0 };
const int dx[4] = { 0, 1, -1, 0 };
const int dy[4] = { 1, 0, 0, -1 };

/*** corner settings ***/
const int cornerDirMap[4] = { 4, 1, 3, 2 };

/*** random utils ***/
inline int randint() { return ((rand() << 16) | rand()) & 0x7fffffff; }
inline int random(int l, int r) { return randint() % (r - l + 1) + l; }
inline void initrand() { srand(time(0)); }

/*** WebSocket connetion **/

WsServer Server;

struct _Conn {
	Conn_Handle Handle;

	bool Joined;
	int RoomID;
	int PlayerID;

	//vector<string> nowait_queue;
	vector<string> queue;
	//handle
	_Conn(Conn_Handle Handle) : Handle(Handle), Joined(false), RoomID(0), PlayerID(0) {}
	_Conn() { _Conn(NULL); }

	inline void close() {
		Joined = false;
		/** Close **/
		try {
			Handle->send_close(1000);
		}
		catch (...) {
		}
	}
	inline void send(string msg) {
		/** Send */
		if(Joined) log("[%d] To %d >> %s", RoomID, PlayerID, msg.c_str());
		else log(" >> %s", msg.c_str());

		try {
			auto send_stream = make_shared<WsServer::SendStream>();
			*send_stream << msg;

			Handle->send(send_stream);
		}
		catch (...) {

		}
	}
};

typedef _Conn* PConn;
map<Conn_Handle, _Conn> conn_list;

bool conn_alive(PConn conn) {
	return conn != NULL;
}
/******************************/

struct coord {
	int x, y;

	coord() {}
	coord(int x, int y) : x(x), y(y) {}

	inline bool operator<(const coord &rhs) const {
		return (x == rhs.x) ? (y < rhs.y) : (x < rhs.x);
	}
};

typedef set<coord> CoordSet;
typedef set<coord>::iterator CoordSetIter;

typedef vector<coord> CoordList;
typedef vector<coord>::iterator CoordListIter;

struct Player {
	bool alive;

	int id;
	string name;
	string skin;

	int x, y, dir, nextdir;
	CoordSet path;
	CoordSet land;

	int kills;

	PConn conn;
	Player(int id, string name, string skin, int x, int y, int dir, CoordSet land, PConn conn) :
		alive(true), id(id), name(name), skin(skin), x(x), y(y), dir(dir), nextdir(dir), land(land), kills(0), conn(conn)
	{
	}
	Player() : alive(false), id(0), conn(NULL) {}
};

typedef map<int, Player> PlayerList;
typedef PlayerList::iterator PlayerIter;

struct PlayerScore {
	int land;
	PlayerIter iter;

	PlayerScore(int land, PlayerIter iter): land(land), iter(iter) {}
	inline bool operator<(const PlayerScore &rhs) const {
		return land > rhs.land;
	}
};

typedef vector<PlayerScore> PlayerScoreList;
typedef PlayerScoreList::iterator PlayerScoreListIter;

inline bool compareMark(Player &a, Player &b) {
	return (a.land.size() == b.land.size()) ? (a.path.size() < b.path.size()) : (a.land.size() < b.land.size());
}

struct Map {
	int MAP_W, MAP_H;
	PlayerList Players;
	set<string> SkinUsed;

	int land[MAX_W][MAX_H];
	bool started;
	bool waiting_players;
	bool isGameover;

	Map(int W, int H) : MAP_W(W), MAP_H(H), started(false), waiting_players(false), isGameover(false) {
		memset(land, 0, sizeof(land));
	}
	void reset_map() {
		memset(land, 0, sizeof(land));

		Players.clear();
		SkinUsed.clear();

		started = false;
		waiting_players = false;
		isGameover = false;
	}

	inline void broadcast_msg(string msg) {
		for (PlayerIter it = Players.begin(); it != Players.end(); it++) if (conn_alive(it->second.conn)) {
			it->second.conn->queue.push_back(msg);
		}
	}
	inline void send_msg(int id, string msg) {
		if(conn_alive(Players[id].conn)) Players[id].conn->queue.push_back(msg);
	}
	inline void send_msg_nowait(int id, string msg) {
		Player &p = Players[id];
		if(conn_alive(p.conn)) {
			p.conn->send(msg);
		}
	}
	void send_queue() {
		for (PlayerIter it = Players.begin(); it != Players.end(); it++) if (conn_alive(it->second.conn) && !it->second.conn->queue.empty()) {
			json multi = json::array();
			//concat msg
			for (vector<string>::iterator msg = it->second.conn->queue.begin(); msg != it->second.conn->queue.end(); msg++) {
				multi.push_back(*msg);
			}
			//send messages
			it->second.conn->send("MULTI " + multi.dump());
			//clear queue
			it->second.conn->queue.clear();
		}
	}

	void send_waiting(int id) {
		send_msg(id, "WAITING []");
	}
	void send_start(int id) {
		Player &p = Players[id];
		json msg = {
			{ "pid", p.id },
			{ "width", MAP_W },
			{ "height", MAP_H },
			{ "x", p.x },
			{ "y", p.y }
		};

		//Send immediately
		send_msg_nowait(id, "INITROOM " + msg.dump());
	}
	void send_spawn(int spawn_id) {
		Player &p = Players[spawn_id];
		json msg = {
			{ "pid", p.id },
			{ "name", p.name },
			{ "skin", p.skin },
			{ "skinned", 0 },
			{ "x", p.x },
			{ "y", p.y }
		};

		broadcast_msg("SPAWN " + msg.dump());
	}
	void send_init_other(int id) {
		Player &p = Players[id];

		for (PlayerIter it = Players.begin(); it != Players.end(); it++) {
			Player &other = it->second;

			if (!other.alive) continue;
			//Ignore Self
			if (other.id == id) continue;

			json msg = {
				{ "pid", other.id },
				{ "name", other.name },
				{ "skin", other.skin },
				{ "skinned", 0 },
				{ "x", other.x },
				{ "y", other.y }
			};

			json land = json::array(),
				path = json::array();

			for (CoordSetIter co = other.path.begin(); co != other.path.end(); co++) {
				path.push_back( to_string(co->x) + "," + to_string(co->y) );
			}
			for (CoordSetIter co = other.land.begin(); co != other.land.end(); co++) {
				land.push_back( to_string(co->x) + "," + to_string(co->y) );
			}

			msg["pg"] = land;
			msg["pp"] = path;

			send_msg(id, "INITPLAYER " + msg.dump());
		}
	}
	void send_kill_player(int id) {
		json msg = {
			{ "pid", id }
		};

		broadcast_msg("KILL " + msg.dump());
	}
	void send_move_player(int id, int x, int y, bool inpath, string corner, CoordList &newland) {
		json msg = {
			{ "pid", id },
			{ "x", x },
			{ "y", y }
		};

		if (!newland.empty()) {
			json land = json::array();
			for (CoordListIter co = newland.begin(); co != newland.end(); co++) {
				land.push_back( to_string(co->x) + "," + to_string(co->y) );
			}
			msg["d"] = land;
		}
		else msg["d"] = inpath ? -1 : 0;
		msg["c"] = corner;

		broadcast_msg("MOVE " + msg.dump());
	}
	void send_scores() {
		json msg = json::object();

		int Total = 0;
		for (PlayerIter it = Players.begin(); it != Players.end(); it++) {
			if (!it->second.alive) continue;
			Total += it->second.land.size();
		}
		for (PlayerIter it = Players.begin(); it != Players.end(); it++) {
			if (!it->second.alive) continue;
			msg[to_string(it->second.id)] = (double)it->second.land.size() / Total * 100;
		}

		broadcast_msg("SCORES " + msg.dump());
	}
	void send_game_over(int id, bool is100 = false) {
		Player &p = Players[id];
		json msg = {
			{ "kc", p.kills },
			{ "got100", is100 }
		};

		send_msg_nowait(id, "GAMEOVER " + msg.dump());
	}

	int alive_payers() {
		int PlayerAlive = 0;
		for (PlayerIter it = Players.begin(); it != Players.end(); it++) {
			if (!it->second.alive) continue;
			PlayerAlive++;
		}
		return PlayerAlive;
	}

	void all_game_over(bool is100 = false) {
		send_scores();

		for (PlayerIter it = Players.begin(); it != Players.end(); it++) {
			if (!it->second.alive) continue;
			send_game_over(it->second.id, is100);
		}

		send_queue();
		reset_map();
	}

	void kill_player(int victim, int killer = -1) {
		if (!Players[victim].alive) return;

		//Set not alive
		Players[victim].alive = false;
		//Remove skin
		SkinUsed.erase(Players[victim].skin);

		//Add killer cnt
		if (killer != -1) Players[killer].kills++;

		//Clear victim lands
		for (CoordSetIter co = Players[victim].land.begin(); co != Players[victim].land.end(); co++) {
			land[co->x][co->y] = 0;
		}

		//Send game over
		send_kill_player(victim);
		send_game_over(victim);

		//game is finally over ?
		if (!started) return;
		if (alive_payers() <= 1) isGameover = true;
	}

	/** Recursive updating **/
	bool _occ[MAX_W][MAX_H];
	bool _vis[MAX_W][MAX_H];
	void _dfs(int x, int y) {
		_vis[x][y] = true;

		for (int dir = 0; dir < 4; dir++) {
			int newx = x + dx[dir],
				newy = y + dy[dir];
			if (!_occ[newx][newy] && !_vis[newx][newy] && newx >= 0 && newy >= 0 && newx < MAP_W && newy < MAP_H)
				_dfs(newx, newy);
		}
	}
	void _start_dfs(int x, int y) {
		if(!_occ[x][y] && !_vis[x][y]) _dfs(x, y);
	}

	void update() {
		if (!started) return;

		//Score first move first
		PlayerScoreList player_list;
		for(PlayerIter it = Players.begin(); it != Players.end(); it++) {
			if(!it->second.alive) continue;

			player_list.push_back(PlayerScore(it->second.land.size(), it));
		}
		sort(player_list.begin(), player_list.end());

		//Do move
		for (PlayerScoreListIter list_it = player_list.begin(); list_it != player_list.end(); list_it++) {
			PlayerIter it = list_it->iter;
			Player &player = it->second;

			//Set next direction
			int corner_lastdir = player.dir;

			player.dir = player.nextdir;
			int newx = player.x + dx[player.dir],
				newy = player.y + dy[player.dir];

			//Kill self path & Border comparison
			if ( (newx < 0 || newy < 0 || newx >= MAP_W || newy >= MAP_H) || player.path.count(coord(newx, newy)) ) {
				kill_player(player.id);
				continue;
			}

			//Kill others
			for (PlayerIter other = Players.begin(); other != Players.end(); other++) if (other != it) {
				if (other->second.alive && other->second.path.count(coord(newx, newy))) {
					kill_player(other->second.id, player.id);
				}
			}

			//Pathing
			bool inpath = 0;
			CoordList newland;

			if (land[newx][newy] != player.id) {
				//Path expand
				player.path.insert(coord(newx, newy));
				inpath = 1;
			}
			else if (!player.path.empty()) {
				//Path end
				//Floodfill
				for (int x = 0; x < MAP_W; x++) for (int y = 0; y < MAP_H; y++) {
					_occ[x][y] = (land[x][y] == player.id);
				}
				for (CoordSetIter co = player.path.begin(); co != player.path.end(); co++) {
					_occ[co->x][co->y] = true;
				}

				//Occupy flooded areas
				memset(_vis, 0, sizeof(_vis));
				for (int x = 0; x < MAP_W; x++) {
					_start_dfs(x, 0);
					_start_dfs(x, MAP_H - 1);
				}
				for (int y = 0; y < MAP_H; y++) {
					_start_dfs(0, y);
					_start_dfs(MAP_W - 1, y);
				}

				//Occupy all path & other
				newland.assign(player.path.begin(), player.path.end());
				for (int x = 0; x < MAP_W; x++) for (int y = 0; y < MAP_H; y++) if(!_vis[x][y] && !_occ[x][y])
					newland.push_back( coord(x, y) );

				//Player coord list
				map< coord, int > PlayerPos;
				for (PlayerIter other = Players.begin(); other != Players.end(); other++) if (other != it) {
					if (other->second.alive) PlayerPos[coord(other->second.x, other->second.y)] = other->second.id;
				}
				//Update land status
				for (CoordListIter co = newland.begin(); co != newland.end(); co++) {
					int owner = land[co->x][co->y];
					if (owner != 0 && owner != player.id) {
						Players[owner].land.erase(*co);
					}

					land[co->x][co->y] = player.id;
					player.land.insert(*co);

					//kill other players
					if (PlayerPos.count(*co)) {
						kill_player(PlayerPos[*co], player.id);
					}
				}

				//clear path
				player.path.clear();
			}

			//Move coord
			player.x = newx;
			player.y = newy;

			//Calc corner
			string corner = /*(player.dir == corner_lastdir) ? "" : to_string(cornerDirMap[corner_lastdir]) + to_string(cornerDirMap[player.dir])*/"";
			//Send event
			send_move_player(player.id, player.x, player.y, inpath, corner, newland);

			//check game over
			if (isGameover) break;
		}

		send_scores();
		send_queue();

		/** Remove dead players **/
		stack<int> removings;
		for (PlayerIter it = Players.begin(); it != Players.end(); it++) {
			if (!it->second.alive) {
				if(conn_alive(it->second.conn)) it->second.conn->close();
				removings.push(it->first);
			}
		}
		while(removings.empty()==false){
         	   Players.erase(removings.top());
         	   removings.pop();
   	       }
		
		if (isGameover) all_game_over(true);
	}

	void start_game() {
		if (started) return;

		started = true;
		waiting_players = false;

		for (PlayerIter it = Players.begin(); it != Players.end(); it++) {
			if (!it->second.alive) continue;
			send_start(it->second.id);
			send_spawn(it->second.id);
		}

		send_scores();
		send_queue();
	}

	/*** Player Interaction ***/
	int join_map(string name, string skin, PConn conn) {
		static bool _available[MAX_W][MAX_H];
		static coord _position[MAX_W * MAX_H];

		int playerCount = 0;

		for (int x = 0; x < MAP_W; x++) for (int y = 0; y < MAP_H; y++) _available[x][y] = land[x][y] == 0;
		for (PlayerIter it = Players.begin(); it != Players.end(); it++) {
			if (!it->second.alive) continue;

			for (CoordSetIter co = it->second.land.begin(); co != it->second.land.end(); co++) {
				_available[co->x][co->y] = false;
			}
			for (CoordSetIter co = it->second.path.begin(); co != it->second.path.end(); co++) {
				_available[co->x][co->y] = false;
			}
			playerCount++;
		}
		if (playerCount >= MAX_PLAYER) return 0;

		//Enum
		int posCount = 0;
		for (int x = BORDER_DIST; x < MAP_W - BORDER_DIST; x++) for (int y = BORDER_DIST; y < MAP_H - BORDER_DIST; y++) {
			//Check ok
			bool Ok = true;
			for (int tx = x - PLAYER_DIST; tx <= x + PLAYER_DIST; tx++) for (int ty = y - PLAYER_DIST; ty <= y + PLAYER_DIST; ty++) if (!_available[tx][ty]) Ok = false;
			//Check min dist
			if (Ok) _position[posCount++] = coord(x, y);
		}

		if (!posCount) return 0;

		coord pos = _position[random(0, posCount - 1)];
		//allocate id
		int id;
		do {
			id = random(MIN_ID, MAX_ID);
		} while (Players.count(id));
		//allocate basic lands
		CoordSet lands;
		for (int tx = pos.x - 1; tx <= pos.x + 1; tx++) for (int ty = pos.y - 1; ty <= pos.y + 1; ty++) {
			land[tx][ty] = id;
			lands.insert(coord(tx, ty));
		}
		//set up skin
		if(skin == NO_SKIN) {
			do {
				skin = SKIN_RAND[random(0, SKIN_RAND_SIZE - 1)];
			} while (SkinUsed.count(skin));
		}
		//set up player
		Players[id] = Player(
			id,
			name,
			skin,

			pos.x,
			pos.y,

			random(0, 3),

			lands,

			conn
		);
		SkinUsed.insert(skin);
		//notify spawn
		if (started) {
			send_start(id);
			send_init_other(id);
			send_spawn(id);
			send_scores();
			send_queue();
		}

		//start the game
		if (!started) {
			if (playerCount + 1 >= MIN_PLAYER) start_game();
			else {
				send_waiting(id);
				send_queue();

				waiting_players = true;
			}
		}
		return id;
	}
	void player_set_direction(int id, int dir) {
		if (!started) return;
		if (!Players.count(id)) return;

		Player &p = Players[id];
		if (!p.alive) return;

		//Not current & (opposite when pathing)
		if ( dir != p.dir && ( p.path.empty() || (dir != 3 - p.dir) ) ) {
			p.nextdir = dir;
		}
	}
	void player_close_connection(int id) {
		if (!Players.count(id)) return;

		Player &p = Players[id];
		p.conn = NULL;

		if (!p.alive) return;
		kill_player(p.id);
	}
};

Map *Rooms[MAX_ROOM];
std::mutex MapMutex;

void send_full(PConn conn) {
	conn->send("FULLROOM []");
	conn->close();
}
bool join_game(PConn conn, string name, string skin, int &ret_room, int &ret_id) {
	//Validate Paramters
	bool Valid = true;
	if(name.length() > MAX_NAME_LEN) Valid = false;

	bool Skin_Found = false;
	for (int i = 0; i < SKIN_ALLOWED_SIZE; i++) if(skin == SKIN_ALLOWED[i]) {
		Skin_Found = true;
		break;
	}
	Valid &= Skin_Found;

	if(!Valid) {
		send_full(conn);
		return false;
	}

	//Try to Join Rooms
	MapMutex.lock();

	static int _room_id_list[MAX_ROOM];
	int room_count = 0;

	// First search room that is waiting players
	for (int i = 0; i < MAX_ROOM; i++) if (Rooms[i]->waiting_players)
		_room_id_list[room_count++] = i;
	for (int i = 0; i < MAX_ROOM; i++) if (!Rooms[i]->waiting_players)
		_room_id_list[room_count++] = i;
	//Try to join
	for(int i = 0; i < room_count; i++) {
		int room_id = _room_id_list[i];
		//Deny join with same skin
		if(skin != NO_SKIN && Rooms[room_id]->SkinUsed.count(skin)) continue;
		//Try to join
		ret_id = Rooms[room_id]->join_map(name, skin, conn);
		if (ret_id) {
			ret_room = room_id;

			MapMutex.unlock();
			return true;
		}
	}

	MapMutex.unlock();

	send_full(conn);
	return false;
}
void set_player_direction(int RoomID, int PlayerID, int dir) {
	MapMutex.lock();
	Rooms[RoomID]->player_set_direction(PlayerID, dir);
	MapMutex.unlock();
}
void close_player_connection(int RoomID, int PlayerID) {
	MapMutex.lock();
	Rooms[RoomID]->player_close_connection(PlayerID);
	MapMutex.unlock();
}

void socket_message(PConn conn, string message) {
	int space = message.find(' ');
	if (string::npos == space) {
		// Invalid Message
		log("Invalid Message");
		return;
	}
	string command = message.substr(0, space);
	string paramter_str = message.substr(space + 1);
	json paramter;

	// Try to parse JSON
	try {
		paramter = json::parse(paramter_str);
	}
	catch (...) {
		// Invalid Message
		log("Invalid JSON");
		return;
	}

	try {
		if (conn->Joined) {
			if (command == "KEYPRESS" && paramter.count("key")) {
				int key = paramter["key"].get<int>() - 37;
				if (key >= 0 && key <= 3)
					set_player_direction(conn->RoomID, conn->PlayerID, dirKeyMap[key]);
			}
		}
		else {
			if (command == "PLAY" && paramter.count("username") && paramter.count("skinny")) {
				int RoomID, PlayerID;

				if (join_game(conn, paramter["username"], paramter["skinny"], RoomID, PlayerID)) {
					/* Conn instance taken in map */
					conn->Joined = true;
					conn->RoomID = RoomID;
					conn->PlayerID = PlayerID;
				}
			}
		}
	}
	catch (...) {
		// Invalid Message
		log("Invalid JSON");
		return;
	}
}

void update_map_thread() {
	//do updates
	while (true) {
		try {
			for (int i = 0; i < MAX_ROOM; i++) {
				MapMutex.lock();
				Rooms[i]->update();
				MapMutex.unlock();
			}
		}
		catch (...) {

		}
		std::this_thread::sleep_for(std::chrono::milliseconds(MOVE_TIMER));
	}
}

void server_on_message(shared_ptr<WsServer::Connection> connection, shared_ptr<WsServer::Message> message) {
	_Conn &conn = conn_list[connection];
	string text = message->string();

	if(text.length() > MAX_MSG_LENGTH) {
		log("Msg Length Exceeded.");
		return;
	}

	if(conn.Joined) log(" << %s", text.c_str() );
	else log("[%d] %d << %s", conn.RoomID, conn.PlayerID, text.c_str() );

	socket_message( &conn, text);
}

void server_on_close(shared_ptr<WsServer::Connection> connection, int status, const string & reason) {
	_Conn &conn = conn_list[connection];

	log("Conn closed.");

	if (conn.Joined)
		close_player_connection(conn.RoomID, conn.PlayerID);
	conn_list.erase(connection);
}

void server_on_open(shared_ptr<WsServer::Connection> connection) {
	log("Conn open");

	conn_list[connection] = _Conn(connection);
}

int main() {
	initrand();
	//init rooms
	for (int i = 0; i < MAX_ROOM; i++) Rooms[i] = new Map(MAX_W, MAX_H);

	//init server
	Server.config.port = SERVER_PORT;
	Server.config.address = SERVER_LISTEN;
	Server.config.timeout_request = SERVER_TIMEOUT;

	auto &endpoint = Server.endpoint[SERVER_ENDPOINT];
	endpoint.on_open = server_on_open;
	endpoint.on_close = server_on_close;
	endpoint.on_message = server_on_message;

	//log start
	log("Server started");

	//Start Update Thread
	thread ThreadUpdate(update_map_thread);

	Server.start();
}
