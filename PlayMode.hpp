#include "Mode.hpp"

#include "Connection.hpp"
#include "Game.hpp"
#include "Dialogue.hpp"
#include "TextHB.hpp"

#include <glm/glm.hpp>

#include <vector>
#include <unordered_set>
#include <deque>

struct PlayMode : Mode {
	PlayMode(Client &client);
	virtual ~PlayMode();

	//functions called by main loop:
	virtual bool handle_event(SDL_Event const &, glm::uvec2 const &window_size) override;
	virtual void update(float elapsed) override;
	virtual void draw(glm::uvec2 const &drawable_size) override;

	//----- game state -----

	//input tracking for local player:
	Player::Controls controls;

	//latest game state (from server):
	Game game;

	//last message from server:
	std::string server_message;

	// ---------- UI / Dialogue (kept from your file) ----------
	std::unique_ptr<TextHB> text;
	Dialogue dialog;
	int cur_state = 0;
	int selected = 0;
	bool finished = false;

	// ---------: basic team model ----------
	enum class Team : uint8_t { Red, Blue };
	static Team infer_team_from_color(glm::vec3 rgb) {
		// heuristic: whichever primary is larger wins
		return (rgb.r >= rgb.b) ? Team::Red : Team::Blue;
	}


	std::vector<Team> my_team;
	std::vector<Team> current_team; // if your server encodes this in Game later, hook it up

	// ---------- scores & theme ----------
	int score_red = 0;
	int score_blue = 0;

	struct Theme {
		glm::vec3 bg         = {0.06f, 0.06f, 0.07f};
		glm::vec3 grid       = {0.60f, 0.00f, 0.70f};
		glm::vec3 red        = {0.92f, 0.28f, 0.28f};
		glm::vec3 blue       = {0.28f, 0.48f, 0.95f};
		glm::vec3 allyGlow   = {1.00f, 1.00f, 0.30f};
		glm::vec3 panelBG    = {0.10f, 0.10f, 0.12f};
		glm::vec3 textDim    = {0.80f, 0.80f, 0.82f};
		glm::vec3 textBright = {1.00f, 1.00f, 1.00f};
	};
	enum class ThemeId { Light, Neo, Terminal };
	Theme theme;
	ThemeId theme_id = ThemeId::Neo;

	// switch theme quickly:
	void set_theme(ThemeId id);

	// ---------- NEW: helpers ----------
	void send_controls_message(); // wraps your existing call
	void draw_sidebar(glm::uvec2 const &drawable_size);
	void draw_arena_and_players(glm::uvec2 const &drawable_size);


	void collect_members(std::vector<std::string>& red_names,
	                     std::vector<std::string>& blue_names,
	                     std::unorder_set<std::string>& my_team_names) const;

	// encode small outline for allies:
	glm::u8vec4 color_for_player(glm::vec3 base, bool is_ally) const;

	//connection to server:
	Client &client;

};
