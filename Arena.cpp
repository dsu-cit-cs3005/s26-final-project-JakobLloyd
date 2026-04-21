#include "Arena.h"

#include <iostream>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <cmath>
#include <thread>
#include <chrono>
#include <dlfcn.h>
#include <dirent.h>
#include <cstring>

namespace fs = std::filesystem;

// ── helpers ──────────────────────────────────────────────────────────────────

static int rand_range(int lo, int hi)          // inclusive [lo, hi]
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
        // trim whitespace
        while (!val.empty() && (val.front() == ' ' || val.front() == '\t')) val.erase(val.begin());

        if (key == "Arena_Size")
        {
            std::istringstream ss(val);
            ss >> m_rows >> m_cols;
        }
        else if (key == "Max_Rounds")   { m_max_rounds = std::stoi(val); }
        else if (key == "Sleep_interval") { m_sleep_sec = std::stod(val); }
        else if (key == "Game_State_Live") { m_live = (val == "true"); }
        else if (key == "Flamethrowers") { m_num_flames = std::stoi(val); }
        else if (key == "Pits")          { m_num_pits   = std::stoi(val); }
        else if (key == "Mounds")        { m_num_mounds = std::stoi(val); }
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
    return true; // symbols or 'X'
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
        int placed = 0;
        int tries  = 0;
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

void Arena::load_robots()
{
    const std::string robot_dir = "robots";

    // Gather .cpp files
    std::vector<std::string> cpp_files;
    DIR* dir = opendir(robot_dir.c_str());
    if (!dir)
    {
        std::cerr << "No 'robots' directory found. Place Robot_*.cpp files in ./robots/\n";
        return;
    }
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr)
    {
        std::string name = entry->d_name;
        if (name.rfind("Robot_", 0) == 0 && name.size() > 4 &&
            name.substr(name.size() - 4) == ".cpp")
        {
            cpp_files.push_back(robot_dir + "/" + name);
        }
    }
    closedir(dir);

    const std::string symbols = "@$#!%^&*+=~";
    int sym_idx = 0;

    using SummaryFn = const char* (*)();

    for (const auto& filepath : cpp_files)
    {
        // derive .so name
        std::string base = filepath.substr(0, filepath.size() - 4); // strip .cpp
        std::string so   = base + ".so";

        std::string cmd = "g++ -shared -fPIC -o " + so + " " + filepath +
                  " RobotBase_pic.o -I. -std=c++20 2>&1";
        std::cout << "Compiling " << filepath << " ...\n";
        if (std::system(cmd.c_str()) != 0)
        {
            std::cerr << "  Failed to compile " << filepath << '\n';
            continue;
        }

        void* handle = dlopen(so.c_str(), RTLD_LAZY);
        if (!handle) { std::cerr << "  dlopen failed: " << dlerror() << '\n'; continue; }

        RobotFactory create_fn = (RobotFactory)dlsym(handle, "create_robot");
        if (!create_fn) { std::cerr << "  create_robot not found\n"; dlclose(handle); continue; }

        SummaryFn summary_fn = (SummaryFn)dlsym(handle, "robot_summary");
        if (summary_fn)
        {
            const char* s = summary_fn();
            if (s) std::cout << "  Summary: " << s << '\n';
        }

        RobotBase* robot = create_fn();
        if (!robot) { std::cerr << "  create_robot returned null\n"; dlclose(handle); continue; }

        char sym = (sym_idx < (int)symbols.size()) ? symbols[sym_idx++] : '?';
        robot->m_name      = filepath;
        robot->m_character = sym;
        robot->set_boundaries(m_rows, m_cols);

        m_robots.push_back({ robot, handle, sym, true });
        std::cout << "  Loaded robot '" << filepath << "' as '" << sym << "'\n";
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

    // column headers
    std::cout << "    ";
    for (int c = 0; c < m_cols; ++c)
        std::cout << std::to_string(c).substr(0, 2) << " ";
    std::cout << '\n';

    for (int r = 0; r < m_rows; ++r)
    {
        // row number
        std::string rnum = std::to_string(r);
        if (rnum.size() == 1) rnum = " " + rnum;
        std::cout << rnum << "  ";

        for (int c = 0; c < m_cols; ++c)
        {
            char ch = m_board[r][c];
            // robots shown as R<sym>
            if (ch != '.' && ch != 'F' && ch != 'P' && ch != 'M' && ch != 'X')
                std::cout << "R" << ch << " ";
            else if (ch == 'X')
                std::cout << "X  ";
            else
                std::cout << ch << "  ";
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
            std::cout << re.robot->m_name << " " << re.symbol << " - is out\n";
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
        // don't add self
        if (r == my_r && c == my_c) return;
        results.push_back(RadarObj(ch, r, c));
    };

    if (direction == 0)
    {
        // surrounding 8 cells
        for (int dr = -1; dr <= 1; ++dr)
            for (int dc = -1; dc <= 1; ++dc)
                if (dr != 0 || dc != 0)
                    add_cell(my_r + dr, my_c + dc);
        return results;
    }

    // directional scan: ray 3 cells wide
    int dr = directions[direction].first;
    int dc = directions[direction].second;

    // perpendicular offsets for the 3-wide beam
    // If moving vertically (dc==0), spread horizontally; else spread vertically
    int pr, pc; // perpendicular unit
    if (dc == 0) { pr = 0; pc = 1; }
    else if (dr == 0) { pr = 1; pc = 0; }
    else { pr = 0; pc = 0; } // diagonal: single-cell wide

    // scan from robot outward to arena edge
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
              << " damage (armor reduced). Health: " << remaining << '\n';

    if (remaining <= 0 && target.alive)
    {
        target.alive = false;
        int r, c;
        target.robot->get_current_location(r, c);
        // Only mark X if not on a pit/flamethrower obstacle (obstacle trumped by dead robot)
        m_board[r][c] = 'X';
        std::cout << "    " << target.robot->m_name << " is DESTROYED!\n";
    }
}

// ── shooting handlers ─────────────────────────────────────────────────────────

void Arena::apply_railgun_shot(RobotEntry& shooter, int shot_row, int shot_col)
{
    int my_r, my_c;
    shooter.robot->get_current_location(my_r, my_c);

    // direction vector
    int dr = shot_row - my_r;
    int dc = shot_col - my_c;
    // normalise to unit step
    int steps = std::max(std::abs(dr), std::abs(dc));
    if (steps == 0) return;
    int ur = dr / steps;
    int uc = dc / steps;

    int r = my_r + ur;
    int c = my_c + uc;
    while (in_bounds(r, c))
    {
        RobotEntry* hit = robot_at(r, c);
        if (hit && hit->alive)
        {
            int dmg = rand_range(10, 20);
            std::cout << "  Railgun hits " << hit->robot->m_name << " at (" << r << "," << c << ")\n";
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
    int steps = std::max(std::abs(dr), std::abs(dc));
    if (steps == 0) return;
    int ur = (dr == 0) ? 0 : dr / std::abs(dr);
    int uc = (dc == 0) ? 0 : dc / std::abs(dc);

    for (int s = 1; s <= 4; ++s)
    {
        int cr = my_r + ur * s;
        int cc = my_c + uc * s;
        for (int pr = -1; pr <= 1; ++pr)
        {
            int tr = cr + pr;
            int tc = cc + ((ur == 0) ? 0 : 0); // spread perpendicular
            // spread 3 wide perpendicular to travel direction
            for (int pc2 = -1; pc2 <= 1; ++pc2)
            {
                int ftr = cr + (uc == 0 ? pr : 0);
                int ftc = cc + (ur == 0 ? pr : 0);
                if (pr == 0 && pc2 != 0) continue; // handled by inner offsets

                (void)tr; (void)tc; (void)pc2;
                if (!in_bounds(ftr, ftc)) continue;
                RobotEntry* hit = robot_at(ftr, ftc);
                if (hit && hit->alive)
                {
                    int dmg = rand_range(30, 50);
                    std::cout << "  Flamethrower hits " << hit->robot->m_name
                              << " at (" << ftr << "," << ftc << ")\n";
                    deal_damage(*hit, dmg);
                }
            }
        }
    }
}

void Arena::apply_hammer_shot(RobotEntry& shooter, int shot_row, int shot_col)
{
    // Hammer hits one adjacent cell
    int my_r, my_c;
    shooter.robot->get_current_location(my_r, my_c);

    if (std::abs(shot_row - my_r) > 1 || std::abs(shot_col - my_c) > 1) return;
    if (!in_bounds(shot_row, shot_col)) return;

    RobotEntry* hit = robot_at(shot_row, shot_col);
    if (hit && hit->alive)
    {
        int dmg = rand_range(50, 60);
        std::cout << "  Hammer SLAMS " << hit->robot->m_name
                  << " at (" << shot_row << "," << shot_col << ")\n";
        deal_damage(*hit, dmg);
    }
}

void Arena::apply_grenade_shot(RobotEntry& shooter, int shot_row, int shot_col)
{
    if (shooter.robot->get_grenades() <= 0)
    {
        std::cout << "  Out of grenades!\n";
        return;
    }
    shooter.robot->decrement_grenades();

    // 3x3 blast
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
            std::cout << "  Grenade hits " << hit->robot->m_name
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
        case railgun:      std::cout << "railgun"; apply_railgun_shot(shooter, shot_row, shot_col);      break;
        case flamethrower: std::cout << "flamethrower"; apply_flamethrower_shot(shooter, shot_row, shot_col); break;
        case hammer:       std::cout << "hammer"; apply_hammer_shot(shooter, shot_row, shot_col);        break;
        case grenade:      std::cout << "grenade launcher"; apply_grenade_shot(shooter, shot_row, shot_col); break;
    }
    std::cout << " at (" << shot_row << "," << shot_col << ")\n";
}

// ── flamethrower obstacle ─────────────────────────────────────────────────────

void Arena::apply_flamethrower_obstacle(RobotEntry& re)
{
    int dmg = rand_range(30, 50);
    std::cout << "  " << re.robot->m_name << " walks through a FLAMETHROWER obstacle!\n";
    deal_damage(re, dmg);
}

// ── movement ──────────────────────────────────────────────────────────────────

void Arena::handle_move(RobotEntry& re, int direction, int distance)
{
    if (direction < 1 || direction > 8) return;
    if (re.robot->get_move_speed() == 0)
    {
        std::cout << "  " << re.robot->m_name << " is trapped (pit) and cannot move.\n";
        return;
    }

    int capped = std::min(distance, re.robot->get_move_speed());
    int dr = directions[direction].first;
    int dc = directions[direction].second;

    int cur_r, cur_c;
    re.robot->get_current_location(cur_r, cur_c);

    // Clear current cell (restore underlying obstacle if any — we track robots separately)
    m_board[cur_r][cur_c] = '.';

    int new_r = cur_r;
    int new_c = cur_c;

    for (int step = 0; step < capped; ++step)
    {
        int nr = new_r + dr;
        int nc = new_c + dc;

        if (!in_bounds(nr, nc)) break; // hit wall

        char cell = m_board[nr][nc];

        if (cell == 'M') // mound: stop before it
        {
            std::cout << "  " << re.robot->m_name << " blocked by Mound at ("
                      << nr << "," << nc << ")\n";
            break;
        }

        if (cell == 'X' || is_robot_cell(cell)) // dead or live robot
        {
            std::cout << "  " << re.robot->m_name << " blocked by robot at ("
                      << nr << "," << nc << ")\n";
            break;
        }

        if (cell == 'P') // pit: land on it, stop
        {
            new_r = nr;
            new_c = nc;
            re.robot->move_to(new_r, new_c);
            re.robot->disable_movement();
            m_board[new_r][new_c] = re.symbol;
            std::cout << "  " << re.robot->m_name << " falls into a PIT at ("
                      << new_r << "," << new_c << ")!\n";
            return;
        }

        if (cell == 'F') // flamethrower: pass through, take damage
        {
            new_r = nr;
            new_c = nc;
            re.robot->move_to(new_r, new_c);
            apply_flamethrower_obstacle(re);
            if (!re.alive)
            {
                m_board[new_r][new_c] = 'X'; // robot dies on the flame cell, flame gone
                return;
            }
            // restore F after passing (robot continues)
            // we'll re-draw F after placing robot temporarily
            // just continue movement; final position will overwrite
        }
        else
        {
            new_r = nr;
            new_c = nc;
        }
    }

    re.robot->move_to(new_r, new_c);
    m_board[new_r][new_c] = re.symbol;
    std::cout << "  " << re.robot->m_name << " moves to ("
              << new_r << "," << new_c << ")\n";
}

// ── sleep ──────────────────────────────────────────────────────────────────────

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

    for (int round = 1; round <= m_max_rounds; ++round)
    {
        for (auto& re : m_robots)
        {
            print_board(round);
            print_stats();

            // Check winner before this robot's turn
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

            if (!re.alive) continue;

            // ── radar ──
            int radar_dir = 0;
            re.robot->get_radar_direction(radar_dir);
            auto radar_results = do_radar(re, radar_dir);
            re.robot->process_radar_results(radar_results);

            // ── shoot or move ──
            int shot_r = 0, shot_c = 0;
            if (re.robot->get_shot_location(shot_r, shot_c))
            {
                handle_shot(re, shot_r, shot_c);
            }
            else
            {
                int move_dir = 0, move_dist = 0;
                re.robot->get_move_direction(move_dir, move_dist);
                if (move_dir != 0 && move_dist > 0)
                    handle_move(re, move_dir, move_dist);
            }

            sleep_interval();
        }
    }

    std::cout << "\n*** MAX ROUNDS REACHED ***\n";
    print_stats();
}
