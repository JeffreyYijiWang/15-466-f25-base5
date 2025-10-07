//help with ChatGPT-4
// ServerGame.cpp
#include "ServerGame.hpp"
#include <sstream>
#include <cctype>

ServerGame::ServerGame(Server& server) : server(server) {}

void ServerGame::poll(double timeout_seconds) {
    server.poll([&](Connection* c, Connection::Event ev){
        switch (ev) {
        case Connection::OnOpen:  on_open(c);  break;
        case Connection::OnRecv:  on_recv(c);  break;
        case Connection::OnClose: on_close(c); break;
        }
    }, timeout_seconds);
}

void ServerGame::on_open(Connection* c) {
    // nothing yet; wait for JOIN <name>
    send_line(c, "HELLO enter 'JOIN <name>'");
}

void ServerGame::on_close(Connection* c) {
    remove_player_by_conn(c);
}

void ServerGame::on_recv(Connection* c) {
    // process all complete lines:
    while (true) {
        std::string line;
        if (!take_line(c->recv_buffer, &line)) break;

        // trim CR:
        if (!line.empty() && line.back()=='\r') line.pop_back();

        std::istringstream iss(line);
        std::string cmd; iss >> cmd;
        for (auto& ch : cmd) ch = std::toupper(ch);

        if (cmd == "JOIN") {
            std::string name; std::getline(iss, name);
            if (!name.empty() && name[0]==' ') name.erase(0,1);
            if (name.empty()) name = "Player";
            add_player(c, name);
        } else if (cmd == "MOVE") {
            int dx=0, dy=0; iss >> dx >> dy;
            try_handle_move(c, dx, dy);
        } else if (cmd == "PING") {
            send_line(c, "PONG");
        } else {
            send_line(c, "ERR unknown");
        }
    }
}

void ServerGame::send_line(Connection* c, const std::string& line) {
    auto& out = c->send_buffer;
    out.insert(out.end(), line.begin(), line.end());
    out.push_back('\n');
}
void ServerGame::broadcast(const std::string& line) {
    for (auto& con : server.connections) {
        if (con) send_line(&con, line);
    }
}

bool ServerGame::take_line(std::vector<char>& buf, std::string* out) {
    auto it = std::find(buf.begin(), buf.end(), '\n');
    if (it == buf.end()) return false;
    out->assign(buf.begin(), it);
    buf.erase(buf.begin(), it+1);
    return true;
}

Team ServerGame::random_team() {
    std::uniform_int_distribution<int> d(0,1);
    return d(rng)==0 ? Team::Red : Team::Blue;
}

void ServerGame::add_player(Connection* c, std::string name) {
    // if already present (rejoin), ignore
    for (auto& [id,p] : state.players) {
        if (p.conn == c) return;
    }
    Player p;
    p.id = next_player_id++;
    p.team = random_team();
    p.x = state.cols/2;
    p.y = state.rows/2;
    p.connected = true;
    p.conn = c;
    p.name = std::move(name);

    state.players.emplace(p.id, p);
    if (p.team == Team::Red) state.team_red.insert(p.id); else state.team_blue.insert(p.id);

    // welcome + current world:
    std::ostringstream w;
    w << "WELCOME " << p.id << ' ' << (p.team==Team::Red ? "RED" : "BLUE") << ' '
      << state.rows << ' ' << state.cols << ' '
      << (state.current_team==Team::Red ? "RED" : "BLUE") << ' ' << state.turn_number;
    send_line(c, w.str());

    // tell this client about all players:
    for (auto& [oid, op] : state.players) {
        std::ostringstream s; s << "SPAWN " << oid << ' ' << (op.team==Team::Red?"RED":"BLUE") << ' ' << op.x << ' ' << op.y << " " << op.name;
        send_line(c, s.str());
    }

    // broadcast new player:
    std::ostringstream s; s << "SPAWN " << p.id << ' ' << (p.team==Team::Red?"RED":"BLUE") << ' ' << p.x << ' ' << p.y << " " << p.name;
    broadcast(s.str());
}

void ServerGame::remove_player_by_conn(Connection* c) {
    uint32_t remove_id = 0;
    for (auto& [id,p] : state.players) {
        if (p.conn == c) { remove_id = id; break; }
    }
    if (!remove_id) return;

    Player& p = state.players.at(remove_id);
    if (p.team==Team::Red) state.team_red.erase(remove_id); else state.team_blue.erase(remove_id);
    state.players.erase(remove_id);

    std::ostringstream s; s << "LEAVE " << remove_id;
    broadcast(s.str());

    // if a team is empty, just pass turns to the other team:
    if (state.team_red.empty() && !state.team_blue.empty()) { state.current_team = Team::Blue; }
    if (state.team_blue.empty() && !state.team_red.empty()) { state.current_team = Team::Red; }

    // if everyone on current team had moved (or was removed), advance:
    if (is_team_done(state.current_team)) next_turn();
}

void ServerGame::try_handle_move(Connection* c, int dx, int dy) {
    // find player by conn:
    Player* me = nullptr;
    for (auto& [id,p] : state.players) if (p.conn == c) { me = &p; break; }
    if (!me) { send_line(c, "ERR not_joined"); return; }

    if (me->team != state.current_team) { send_line(c, "ERR not_your_team"); return; }
    if (me->moved_this_turn) { send_line(c, "ERR already_moved"); return; }

    int nx = me->x + dx;
    int ny = me->y + dy;
    if (!in_bounds(ny, nx)) { send_line(c, "ERR out_of_bounds"); return; }

    me->x = nx; me->y = ny; me->moved_this_turn = true;

    std::ostringstream s; s << "MOVE " << me->id << ' ' << me->x << ' ' << me->y;
    broadcast(s.str());

    if (is_team_done(me->team)) next_turn();
}

bool ServerGame::is_team_done(Team t) const {
    const auto& set = (t==Team::Red) ? state.team_red : state.team_blue;
    if (set.empty()) return true; // trivially done
    for (uint32_t id : set) {
        auto it = state.players.find(id);
        if (it == state.players.end()) continue;
        if (!it->second.moved_this_turn) return false;
    }
    return true;
}

void ServerGame::next_turn() {
    // reset moved flags on the *other* team and advance:
    Team next = (state.current_team==Team::Red ? Team::Blue : Team::Red);
    for (auto& [id,p] : state.players) {
        if (p.team == next) p.moved_this_turn = false;
    }
    state.current_team = next;
    ++state.turn_number;

    std::ostringstream s; s << "TURN " << (state.current_team==Team::Red?"RED":"BLUE") << ' ' << state.turn_number;
    broadcast(s.str());
}
