// ServerGame.hpp
#pragma once
#include <unordered_map>
#include <unordered_set>
#include <list>
#include <random>
#include <string>
#include "Connection.hpp"

enum class Team : uint8_t { Red = 0, Blue = 1 };

struct Player {
    uint32_t id;
    Team team;
    int x = 0, y = 0;               // grid position
    bool connected = true;
    bool moved_this_turn = false;
    Connection* conn = nullptr;     // not owned; points into Server::connections list
    std::string name;
};

struct GameState {
    int rows = 10, cols = 16;       // grid dimensions (customize)
    Team current_team = Team::Red;
    uint64_t turn_number = 1;

    std::unordered_map<uint32_t, Player> players;      // id -> player
    std::unordered_set<uint32_t> team_red;             // ids
    std::unordered_set<uint32_t> team_blue;            // ids
};

struct ServerGame {
    explicit ServerGame(Server& server);

    void poll(double timeout_seconds);

private:
    Server& server;
    GameState state;

    // id bookkeeping:
    uint32_t next_player_id = 1;
    std::mt19937 rng{std::random_device{}()};

    // --- event handlers from poll_connections ---
    void on_open(Connection* c);
    void on_close(Connection* c);
    void on_recv(Connection* c);

    // --- protocol helpers ---
    void send_line(Connection* c, const std::string& line);
    void broadcast(const std::string& line);
    static bool take_line(std::vector<char>& buf, std::string* out); // pop one \n-terminated line

    // --- game logic ---
    Team random_team();
    void add_player(Connection* c, std::string name);
    void remove_player_by_conn(Connection* c);
    void try_handle_move(Connection* c, int dx, int dy);
    bool is_team_done(Team t) const;
    void next_turn();
    bool in_bounds(int r, int c) const { return r>=0 && r<state.rows && c>=0 && c<state.cols; }
};
