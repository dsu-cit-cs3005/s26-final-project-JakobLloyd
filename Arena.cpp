#include "Arena.h"

#include <iostream>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <cmath>
#include <thread>
#include <chrono>
#include <dlfcn.h>
#include <dirent.h>
#include <cstring>

// ── helpers ──────────────────────────────────────────────────────────────────

static int rand_range(int lo, int hi)
{
    return lo + std::rand() % (hi - lo + 1);
}

// ── ctor / dtor ──────────────────────────────────────────────────────────────

Arena::Arena()
{
    std::srand(static_cast<unsigned>(std::time(nullptr)));
}

Arena::~Arena()
{
    for (auto& re : m_robots)
    {
        delete re.robot;
        if (re.handle) dlclose(re.handle);
    }
}

// ── config ───────────────────────────────────────────────────────────────────

bool Arena::load_config(const std::string& config_file)
{
    std::ifstream f(config_file);
    if (!f)
    {
        std::cerr << "Cannot open config file: " << config_file << '\n';
        return false;
    }

    std::string line;
    while (std::getline(f, line))
    {
        if (line.empty() || line[0] == '#') continue;
        auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string key = line.substr(0, colon);
        std::string val = line.substr(colon + 1);
        while (!val.empty() && (val.front() == ' ' || val.front() == '\t'))
            val.erase(val.begin());

        if (key == "Arena_Size")
        {
            std::istringstream ss(val);
            ss >> m_rows >> m_cols;
        }
        else if (key == "Max_Rounds")      { m_max_rounds = std::stoi(val); }
        else if (key == "Sleep_interval")  { m_sleep_sec  = std::stod(val); }
        else if (key == "Game_State_Live") { m_live       = (val == "true"); }
        else if (key == "Flamethrowers")   { m_num_flames = std::stoi(val); }
        else if (key == "Pits")            { m_num_pits   = std::stoi(val); }
        else if (key == "Mounds")          { m_num_mounds = std::stoi(val); }
    }
    return true;
}

// ── board helpers ─────────────────────────────────────────────────────────────

bool Arena::in_bounds(int r, int c) const
{
    return r >= 0 && r < m_rows && c >= 0 && c < m_cols;
}

bool Arena::is_obstacle_cell(char ch) const
{
    return ch == 'M';
}

bool Arena::is_robot_cell(char ch) const
{
    if (ch == '.' || ch == 'F' || ch == 'P' || ch == 'M') return false;
    return true;
}

RobotEntry* Arena::robot_at(int r, int c)
{
    char ch = m_board[r][c];
    if (!is_robot_cell(ch)) return nullptr;
    for (auto& re : m_robots)
    {
        int rr, rc;
        re.robot->get_current_location(rr, rc);
        if (rr == r && rc == c) return &re;
    }
    return nullptr;
}

void Arena::update_board_symbol(RobotEntry& re)
{
    int r, c;
    re.robot->get_current_location(r, c);
    m_board[r][c] = re.alive ? re.symbol : 'X';
}

// ── obstacle placement ────────────────────────────────────────────────────────

void Arena::place_obstacles()
{
    m_board.assign(m_rows, std::vector<char>(m_cols, '.'));

    auto place = [&](char ch, int count)
    {
        int placed = 0, tries = 0;
        while (placed < count && tries < 10000)
        {
            ++tries;
            int r = rand_range(0, m_rows - 1);
            int c = rand_range(0, m_cols - 1);
            if (m_board[r][c] == '.') { m_board[r][c] = ch; ++placed; }
        }
    };

    place('F', m_num_flames);
    place('P', m_num_pits);
    place('M', m_num_mounds);
}

// ── robot loading ─────────────────────────────────────────────────────────────

// Collect Robot_*.cpp files from a given directory
static void collect_robot_files(const std::string& dir, std::vector<std::string>& out)
{
    DIR* d = opendir(dir.c_str());
    if (!d) return;

    struct dirent* entry;
    while ((entry = readdir(d)) != nullptr)
    {
        std::string name = entry->d_name;
        if (name.rfind("Robot_", 0) == 0 &&
            name.size() > 4 &&
            name.substr(name.size() - 4) == ".cpp")
        {
            std::string full = (dir == ".") ? name : dir + "/" + name;
            // avoid duplicates
            bool found = false;
            for (const auto& f : out)
                if (f == full) { found = true; break; }
            if (!found) out.push_back(full);
        }
    }
    closedir(d);
}

void Arena::load_robots()
{
    std::vector<std::string> cpp_files;

    // Search current directory first (grader drops robots here)
    collect_robot_files(".", cpp_files);
    // Also search robots/ subdirectory
    collect_robot_files("robots", cpp_files);

    if (cpp_files.empty())
    {
        std::cerr << "No Robot_*.cpp files found in ./ or ./robots/\n";
        return;
    }

    using SummaryFn = const char* (*)();

    const std::string symbols = "@$#!%^&*+=~";
    int sym_idx = 0;

    for (const auto& filepath : cpp_files)
    {
        // Build .so path alongside the .cpp
        std::string so = filepath.substr(0, filepath.size() - 4) + ".so";

        // RobotBase_pic.o is always in the current working directory
        std::string cmd = "g++ -shared -fPIC -o " + so + " " + filepath +
                          " ./RobotBase_pic.o -I. -std=c++20 2>&1";

        std::cout << "Compiling " << filepath << " ...\n";
        if (std::system(cmd.c_str()) != 0)
        {
            std::cerr << "  Failed to compile " << filepath << '\n';
            continue;
        }

        void* handle = dlopen(so.c_str(), RTLD_LAZY);
        if (!handle)
        {
            std::cerr << "  dlopen failed: " << dlerror() << '\n';
            continue;
        }

        RobotFactory create_fn = (RobotFactory)dlsym(handle, "create_robot");
        if (!create_fn)
        {
            std::cerr << "  create_robot not found: " << dlerror() << '\n';
            dlclose(handle);
            continue;
        }

        SummaryFn summary_fn = (SummaryFn)dlsym(handle, "robot_summary");
        if (!summary_fn)
        {
            std::cerr << "  robot_summary not found: " << dlerror() << '\n';
            dlclose(handle);
            continue;
        }

        const char* summary = summary_fn();
        if (!summary || std::strlen(summary) == 0 || std::strlen(summary) > 50)
        {
            std::cerr << "  Invalid robot_summary for " << filepath << '\n';
            dlclose(handle);
            continue;
        }
        std::cout << "  Summary: " << summary << '\n';

        RobotBase* robot = create_fn();
        if (!robot)
        {
            std::cerr << "  create_robot returned null\n";
            dlclose(handle);
            continue;
        }

        char sym = (sym_idx < (int)symbols.size()) ? symbols[sym_idx++] : '?';

        // Extract just the robot name from the filename
        std::string base = filepath;
        auto slash = base.rfind('/');
        if (slash != std::string::npos) base = base.substr(slash + 1);
        // strip Robot_ prefix and .cpp suffix
        std::string rname = base.substr(6, base.size() - 10);

        robot->m_name      = rname;
        robot->m_character = sym;
        robot->set_boundaries(m_rows, m_cols);

        m_robots.push_back({ robot, handle, sym, true });
        std::cout << "  Loaded: " << rname << " as '" << sym << "'\n";
    }
}

// ── robot placement ───────────────────────────────────────────────────────────

void Arena::place_robots()
{
    for (auto& re : m_robots)
    {
        int tries = 0;
        while (tries++ < 10000)
        {
            int r = rand_range(0, m_rows - 1);
            int c = rand_range(0, m_cols - 1);
            if (m_board[r][c] == '.')
            {
                re.robot->move_to(r, c);
                m_board[r][c] = re.symbol;
                break;
            }
        }
    }
}

// ── board printing ────────────────────────────────────────────────────────────

void Arena::print_board(int round) const
{
    std::cout << "\n         =========== starting round " << round << " ===========\n\n";

    // Column headers
    std::cout << "     ";
    for (int c = 0; c < m_cols; ++c)
    {
        if (c < 10)
            std::cout << " " << c << " ";
        else
            std::cout << c << " ";
    }
    std::cout << '\n';

    for (int r = 0; r < m_rows; ++r)
    {
        if (r < 10) std::cout << " ";
        std::cout << r << "  ";

        for (int c = 0; c < m_cols; ++c)
        {
            char ch = m_board[r][c];
            if (ch == '.')       std::cout << " . ";
            else if (ch == 'F')  std::cout << " F ";
            else if (ch == 'P')  std::cout << " P ";
            else if (ch == 'M')  std::cout << " M ";
            else if (ch == 'X')  std::cout << "X  ";
            else                 std::cout << "R" << ch << " ";
        }
        std::cout << '\n';
    }
    std::cout << '\n';
}

void Arena::print_stats() const
{
    for (const auto& re : m_robots)
    {
        if (!re.alive)
            std::cout << re.robot->m_name << " [" << re.symbol << "] - is out\n";
        else
            std::cout << re.robot->print_stats() << '\n';
    }
    std::cout << '\n';
}

// ── win check ─────────────────────────────────────────────────────────────────

int Arena::count_living() const
{
    int n = 0;
    for (const auto& re : m_robots) if (re.alive) ++n;
    return n;
}

RobotEntry* Arena::find_winner()
{
    RobotEntry* w = nullptr;
    int n = 0;
    for (auto& re : m_robots)
        if (re.alive) { w = &re; ++n; }
    return (n == 1) ? w : nullptr;
}

// ── radar ─────────────────────────────────────────────────────────────────────

std::vector<RadarObj> Arena::do_radar(RobotEntry& re, int direction) const
{
    std::vector<RadarObj> results;
    int my_r, my_c;
    re.robot->get_current_location(my_r, my_c);

    auto add_cell = [&](int r, int c)
    {
        if (!in_bounds(r, c)) return;
        char ch = m_board[r][c];
        if (ch == '.') return;
        if (r == my_r && c == my_c) return; // skip self
        results.push_back(RadarObj(ch, r, c));
    };

    if (direction == 0)
    {
        // All 8 surrounding cells
        for (int dr = -1; dr <= 1; ++dr)
            for (int dc = -1; dc <= 1; ++dc)
                if (dr != 0 || dc != 0)
                    add_cell(my_r + dr, my_c + dc);
        return results;
    }

    if (direction < 1 || direction > 8) return results;

    int dr = directions[direction].first;
    int dc = directions[direction].second;

    // Perpendicular spread for 3-wide beam (cardinal directions only)
    int pr = 0, pc = 0;
    if (dc == 0 && dr != 0) { pr = 0; pc = 1; }      // vertical movement -> spread horizontal
    else if (dr == 0 && dc != 0) { pr = 1; pc = 0; } // horizontal movement -> spread vertical
    // diagonal: single cell wide (pr=pc=0)

    int step_r = my_r + dr;
    int step_c = my_c + dc;

    while (in_bounds(step_r, step_c))
    {
        add_cell(step_r, step_c);
        if (pr != 0 || pc != 0)
        {
            add_cell(step_r + pr, step_c + pc);
            add_cell(step_r - pr, step_c - pc);
        }
        step_r += dr;
        step_c += dc;
    }

    return results;
}

// ── damage ────────────────────────────────────────────────────────────────────

void Arena::deal_damage(RobotEntry& target, int base_damage)
{
    int armor = target.robot->get_armor();
    double reduction = armor * 0.10;
    int actual = static_cast<int>(base_damage * (1.0 - reduction));
    if (actual < 1) actual = 1;

    target.robot->reduce_armor(1);
    int remaining = target.robot->take_damage(actual);

    std::cout << "    " << target.robot->m_name << " takes " << actual
              << " damage. Health now: " << remaining << '\n';

    if (remaining <= 0 && target.alive)
    {
        target.alive = false;
        int r, c;
        target.robot->get_current_location(r, c);
        m_board[r][c] = 'X';
        std::cout << "    *** " << target.robot->m_name << " is DESTROYED! ***\n";
    }
}

// ── shooting ──────────────────────────────────────────────────────────────────

void Arena::apply_railgun_shot(RobotEntry& shooter, int shot_row, int shot_col)
{
    int my_r, my_c;
    shooter.robot->get_current_location(my_r, my_c);

    int dr = shot_row - my_r;
    int dc = shot_col - my_c;
    int steps = std::max(std::abs(dr), std::abs(dc));
    if (steps == 0) return;

    // Normalise to unit step
    int ur = (dr == 0) ? 0 : dr / std::abs(dr);
    int uc = (dc == 0) ? 0 : dc / std::abs(dc);

    int r = my_r + ur;
    int c = my_c + uc;
    while (in_bounds(r, c))
    {
        RobotEntry* hit = robot_at(r, c);
        if (hit && hit->alive)
        {
            int dmg = rand_range(10, 20);
            std::cout << "    Railgun hits " << hit->robot->m_name
                      << " at (" << r << "," << c << ")\n";
            deal_damage(*hit, dmg);
        }
        r += ur;
        c += uc;
    }
}

void Arena::apply_flamethrower_shot(RobotEntry& shooter, int shot_row, int shot_col)
{
    int my_r, my_c;
    shooter.robot->get_current_location(my_r, my_c);

    int dr = shot_row - my_r;
    int dc = shot_col - my_c;

    // Normalise to unit direction
    int ur = (dr == 0) ? 0 : dr / std::abs(dr);
    int uc = (dc == 0) ? 0 : dc / std::abs(dc);

    // Perpendicular spread: 3 wide
    int pr, pc;
    if (uc == 0) { pr = 0; pc = 1; }       // moving vertically, spread horizontal
    else if (ur == 0) { pr = 1; pc = 0; }  // moving horizontally, spread vertical
    else { pr = 0; pc = 0; }               // diagonal: no spread

    // 4 cells deep
    for (int s = 1; s <= 4; ++s)
    {
        int cr = my_r + ur * s;
        int cc = my_c + uc * s;

        for (int spread = -1; spread <= 1; ++spread)
        {
            int tr = cr + pr * spread;
            int tc = cc + pc * spread;
            if (!in_bounds(tr, tc)) continue;

            RobotEntry* hit = robot_at(tr, tc);
            if (hit && hit->alive)
            {
                int dmg = rand_range(30, 50);
                std::cout << "    Flamethrower hits " << hit->robot->m_name
                          << " at (" << tr << "," << tc << ")\n";
                deal_damage(*hit, dmg);
            }
        }
    }
}

void Arena::apply_hammer_shot(RobotEntry& shooter, int shot_row, int shot_col)
{
    int my_r, my_c;
    shooter.robot->get_current_location(my_r, my_c);

    // Must be exactly 1 cell away (including diagonal)
    if (std::abs(shot_row - my_r) > 1 || std::abs(shot_col - my_c) > 1) return;
    if (shot_row == my_r && shot_col == my_c) return;
    if (!in_bounds(shot_row, shot_col)) return;

    RobotEntry* hit = robot_at(shot_row, shot_col);
    if (hit && hit->alive)
    {
        int dmg = rand_range(50, 60);
        std::cout << "    Hammer SLAMS " << hit->robot->m_name
                  << " at (" << shot_row << "," << shot_col << ")\n";
        deal_damage(*hit, dmg);
    }
}

void Arena::apply_grenade_shot(RobotEntry& shooter, int shot_row, int shot_col)
{
    if (shooter.robot->get_grenades() <= 0)
    {
        std::cout << "    Out of grenades!\n";
        return;
    }
    shooter.robot->decrement_grenades();

    // 3x3 blast radius
    for (int dr = -1; dr <= 1; ++dr)
    for (int dc = -1; dc <= 1; ++dc)
    {
        int tr = shot_row + dr;
        int tc = shot_col + dc;
        if (!in_bounds(tr, tc)) continue;

        RobotEntry* hit = robot_at(tr, tc);
        if (hit && hit->alive)
        {
            int dmg = rand_range(10, 40);
            std::cout << "    Grenade hits " << hit->robot->m_name
                      << " at (" << tr << "," << tc << ")\n";
            deal_damage(*hit, dmg);
        }
    }
}

void Arena::handle_shot(RobotEntry& shooter, int shot_row, int shot_col)
{
    WeaponType w = shooter.robot->get_weapon();

    std::cout << "  " << shooter.robot->m_name << " fires ";
    switch (w)
    {
        case railgun:
            std::cout << "railgun at (" << shot_row << "," << shot_col << ")\n";
            apply_railgun_shot(shooter, shot_row, shot_col);
            break;
        case flamethrower:
            std::cout << "flamethrower at (" << shot_row << "," << shot_col << ")\n";
            apply_flamethrower_shot(shooter, shot_row, shot_col);
            break;
        case hammer:
            std::cout << "hammer at (" << shot_row << "," << shot_col << ")\n";
            apply_hammer_shot(shooter, shot_row, shot_col);
            break;
        case grenade:
            std::cout << "grenade launcher at (" << shot_row << "," << shot_col
                      << ") grenades left: " << shooter.robot->get_grenades() << "\n";
            apply_grenade_shot(shooter, shot_row, shot_col);
            break;
    }
}

// ── flamethrower obstacle ─────────────────────────────────────────────────────

void Arena::apply_flamethrower_obstacle(RobotEntry& re)
{
    int dmg = rand_range(30, 50);
    std::cout << "  " << re.robot->m_name
              << " walks through a FLAMETHROWER obstacle! Takes " << dmg << " damage.\n";
    deal_damage(re, dmg);
}

// ── movement ──────────────────────────────────────────────────────────────────

void Arena::handle_move(RobotEntry& re, int direction, int distance)
{
    if (direction < 1 || direction > 8) return;

    if (re.robot->get_move_speed() == 0)
    {
        std::cout << "  " << re.robot->m_name << " is trapped in a pit and cannot move.\n";
        return;
    }

    int capped = std::min(distance, re.robot->get_move_speed());
    int dr = directions[direction].first;
    int dc = directions[direction].second;

    int cur_r, cur_c;
    re.robot->get_current_location(cur_r, cur_c);

    // Clear current cell
    m_board[cur_r][cur_c] = '.';

    int new_r = cur_r;
    int new_c = cur_c;

    for (int step = 0; step < capped; ++step)
    {
        int nr = new_r + dr;
        int nc = new_c + dc;

        if (!in_bounds(nr, nc))
        {
            std::cout << "  " << re.robot->m_name << " hits the arena boundary.\n";
            break;
        }

        char cell = m_board[nr][nc];

        if (cell == 'M')
        {
            std::cout << "  " << re.robot->m_name
                      << " blocked by Mound at (" << nr << "," << nc << ")\n";
            break;
        }

        if (cell == 'X' || is_robot_cell(cell))
        {
            std::cout << "  " << re.robot->m_name
                      << " blocked by robot at (" << nr << "," << nc << ")\n";
            break;
        }

        if (cell == 'P')
        {
            new_r = nr;
            new_c = nc;
            re.robot->move_to(new_r, new_c);
            re.robot->disable_movement();
            m_board[new_r][new_c] = re.symbol;
            std::cout << "  " << re.robot->m_name
                      << " falls into a PIT at (" << new_r << "," << new_c << ")!\n";
            return;
        }

        if (cell == 'F')
        {
            new_r = nr;
            new_c = nc;
            re.robot->move_to(new_r, new_c);
            apply_flamethrower_obstacle(re);

            if (!re.alive)
            {
                // Robot dies on flamethrower cell — mark X, flamethrower gone
                m_board[new_r][new_c] = 'X';
                return;
            }
            // Robot survived — restore F on board, robot continues moving
            m_board[new_r][new_c] = 'F';
            continue;
        }

        // Empty cell
        new_r = nr;
        new_c = nc;
    }

    re.robot->move_to(new_r, new_c);
    m_board[new_r][new_c] = re.symbol;
    std::cout << "  " << re.robot->m_name
              << " moves to (" << new_r << "," << new_c << ")\n";
}

// ── sleep ─────────────────────────────────────────────────────────────────────

void Arena::sleep_interval() const
{
    if (m_live && m_sleep_sec > 0)
    {
        auto ms = static_cast<long long>(m_sleep_sec * 1000);
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    }
}

// ── main game loop ────────────────────────────────────────────────────────────

void Arena::run()
{
    place_obstacles();
    place_robots();

    if (m_robots.empty())
    {
        std::cerr << "No robots loaded. Exiting.\n";
        return;
    }

    std::cout << "\n*** ROBOTWARZ BEGINS! " << m_robots.size() << " robots enter! ***\n";

    for (int round = 1; round <= m_max_rounds; ++round)
    {
        for (size_t i = 0; i < m_robots.size(); ++i)
        {
            auto& re = m_robots[i];

            print_board(round);
            print_stats();

            // Check for winner before each robot's turn
            if (count_living() <= 1)
            {
                RobotEntry* w = find_winner();
                if (w)
                    std::cout << "\n*** WINNER: " << w->robot->m_name
                              << " [" << w->symbol << "] ***\n";
                else
                    std::cout << "\n*** DRAW - all robots destroyed! ***\n";
                return;
            }

            if (!re.alive)
            {
                std::cout << re.robot->m_name << " [" << re.symbol << "] is out - skipping.\n";
                continue;
            }

            std::cout << "--- " << re.robot->m_name << " [" << re.symbol << "]'s turn ---\n";

            // ── 1. radar ──
            int radar_dir = 0;
            re.robot->get_radar_direction(radar_dir);
            std::cout << "  Radar direction: " << radar_dir << '\n';

            auto radar_results = do_radar(re, radar_dir);
            std::cout << "  Radar found " << radar_results.size() << " object(s)\n";

            re.robot->process_radar_results(radar_results);

            // ── 2. shoot OR move ──
            int shot_r = 0, shot_c = 0;
            if (re.robot->get_shot_location(shot_r, shot_c))
            {
                handle_shot(re, shot_r, shot_c);
            }
            else
            {
                int move_dir = 0, move_dist = 0;
                re.robot->get_move_direction(move_dir, move_dist);
                if (move_dir >= 1 && move_dir <= 8 && move_dist > 0)
                    handle_move(re, move_dir, move_dist);
                else
                    std::cout << "  " << re.robot->m_name << " does nothing.\n";
            }

            sleep_interval();
        }
    }

    std::cout << "\n*** MAX ROUNDS REACHED ***\n";
    print_stats();

    // Still declare a winner by health if max rounds hit
    int best_health = -1;
    RobotEntry* best = nullptr;
    for (auto& re : m_robots)
    {
        if (re.alive && re.robot->get_health() > best_health)
        {
            best_health = re.robot->get_health();
            best = &re;
        }
    }
    if (best)
        std::cout << "Most health remaining: " << best->robot->m_name
                  << " with " << best_health << " HP\n";
}
