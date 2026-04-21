#pragma once

#include "RobotBase.h"
#include "RadarObj.h"

#include <string>
#include <vector>
#include <utility>

struct RobotEntry
{
    RobotBase* robot;
    void*      handle;
    char       symbol;
    bool       alive;
};

class Arena
{
public:
    Arena();
    ~Arena();

    bool load_config(const std::string& config_file);
    void load_robots();
    void run();

private:
    // config
    int    m_rows        = 20;
    int    m_cols        = 20;
    int    m_max_rounds  = 10000;
    double m_sleep_sec   = 0.5;
    bool   m_live        = true;
    int    m_num_flames  = 5;
    int    m_num_pits    = 5;
    int    m_num_mounds  = 5;

    // board: '.' empty, 'F' flame, 'P' pit, 'M' mound, robot symbol, 'X' dead robot
    std::vector<std::vector<char>> m_board;

    std::vector<RobotEntry> m_robots;

    // helpers
    void  place_obstacles();
    void  place_robots();
    void  print_board(int round) const;
    void  print_stats() const;

    bool  in_bounds(int r, int c) const;
    bool  is_obstacle_cell(char ch) const;   // M only (blocks movement)
    bool  is_robot_cell(char ch) const;      // live or dead robot symbol

    // radar
    std::vector<RadarObj> do_radar(RobotEntry& re, int direction) const;

    // shooting
    void handle_shot(RobotEntry& shooter, int shot_row, int shot_col);
    void apply_flamethrower_shot(RobotEntry& shooter, int shot_row, int shot_col);
    void apply_railgun_shot(RobotEntry& shooter, int shot_row, int shot_col);
    void apply_hammer_shot(RobotEntry& shooter, int shot_row, int shot_col);
    void apply_grenade_shot(RobotEntry& shooter, int shot_row, int shot_col);
    void deal_damage(RobotEntry& target, int base_damage);

    // movement
    void handle_move(RobotEntry& re, int direction, int distance);

    // win check
    int  count_living() const;
    RobotEntry* find_winner();

    // cell lookup
    RobotEntry* robot_at(int r, int c);
    void        update_board_symbol(RobotEntry& re);

    // obstacle interaction
    void apply_flamethrower_obstacle(RobotEntry& re);

    void sleep_interval() const;
};
