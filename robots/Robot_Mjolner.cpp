#include "RobotBase.h"
#include <vector>
#include <cmath>
#include <limits>

// Mjolner: Max armor (5), min move (2), hammer weapon.
// Strategy: scan all 8 surrounding cells every turn.
// If an enemy is adjacent, SMASH with the hammer.
// Otherwise close in on the last known enemy position.

class Robot_Mjolner : public RobotBase
{
private:
    // Last known enemy position
    int  m_target_r  = -1;
    int  m_target_c  = -1;
    bool m_has_target = false;

    // Scan sweeping direction when no target
    int m_sweep_dir = 1;

public:
    // Move=2, Armor=5, Weapon=hammer  (2+5=7, satisfies constraint)
    Robot_Mjolner() : RobotBase(2, 5, hammer)
    {
        m_name      = "Mjolner";
        m_character = 'M';
    }

    // ── radar ──────────────────────────────────────────────────────────────
    void get_radar_direction(int& radar_direction) override
    {
        // If we have a target, scan surrounding cells to check adjacency
        // Otherwise sweep directionally
        if (m_has_target)
            radar_direction = 0;   // 0 = check all 8 neighbours
        else
        {
            radar_direction = m_sweep_dir;
            m_sweep_dir = (m_sweep_dir % 8) + 1;
        }
    }

    // ── process radar ───────────────────────────────────────────────────────
    void process_radar_results(const std::vector<RadarObj>& results) override
    {
        m_has_target = false;
        int my_r, my_c;
        get_current_location(my_r, my_c);

        // Prefer the closest live robot
        int best_dist = std::numeric_limits<int>::max();

        for (const auto& obj : results)
        {
            if (obj.m_type == 'R' || obj.m_type == 'X') // live robot char or... only 'R' matters
            {
                // Only living robots have type 'R' in the arena's radar
                if (obj.m_type != 'R') continue;

                int dist = std::abs(obj.m_row - my_r) + std::abs(obj.m_col - my_c);
                if (dist < best_dist)
                {
                    best_dist   = dist;
                    m_target_r  = obj.m_row;
                    m_target_c  = obj.m_col;
                    m_has_target = true;
                }
            }
        }
    }

    // ── shoot ───────────────────────────────────────────────────────────────
    bool get_shot_location(int& shot_row, int& shot_col) override
    {
        if (!m_has_target) return false;

        int my_r, my_c;
        get_current_location(my_r, my_c);

        int dr = std::abs(m_target_r - my_r);
        int dc = std::abs(m_target_c - my_c);

        // Hammer only reaches 1 adjacent cell (including diagonals)
        if (dr <= 1 && dc <= 1 && (dr + dc > 0))
        {
            shot_row = m_target_r;
            shot_col = m_target_c;
            return true;
        }

        return false; // target not yet adjacent
    }

    // ── move ────────────────────────────────────────────────────────────────
    void get_move_direction(int& move_direction, int& move_distance) override
    {
        if (!m_has_target)
        {
            // Random wander
            move_direction = (std::rand() % 8) + 1;
            move_distance  = 1;
            return;
        }

        int my_r, my_c;
        get_current_location(my_r, my_c);

        int dr = m_target_r - my_r;
        int dc = m_target_c - my_c;

        // Pick the best cardinal/diagonal direction toward target
        int best_dir  = 0;
        int best_score = std::numeric_limits<int>::max();

        for (int d = 1; d <= 8; ++d)
        {
            int nr = my_r + directions[d].first;
            int nc = my_c + directions[d].second;
            int score = std::abs(m_target_r - nr) + std::abs(m_target_c - nc);
            if (score < best_score)
            {
                best_score = score;
                best_dir   = d;
            }
        }

        move_direction = best_dir;
        move_distance  = get_move_speed(); // use full move speed
    }
};

extern "C" RobotBase* create_robot()
{
    return new Robot_Mjolner();
}

extern "C" const char* robot_summary()
{
    return "Max armor hammer. Closes in and smashes.";
}
