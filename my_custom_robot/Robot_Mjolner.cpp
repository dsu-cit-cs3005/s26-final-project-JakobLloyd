#include "RobotBase.h"
#include <vector>
#include <cmath>
#include <limits>
#include <cstdlib>

class Robot_Mjolner : public RobotBase
{
private:
    int  m_target_r      = -1;
    int  m_target_c      = -1;
    bool m_has_target    = false;
    int  m_sweep_dir     = 1;
    int  m_turns_no_target = 0;

public:
    Robot_Mjolner() : RobotBase(2, 5, hammer)
    {
        m_name      = "Mjolner";
        m_character = 'M';
    }

    void get_radar_direction(int& radar_direction) override
    {
        if (m_has_target)
        {
            // When we have a target lock radar direction 0 to watch surroundings
            radar_direction = 0;
        }
        else
        {
            // Cycle through ALL 8 directions rapidly to find anyone
            radar_direction = m_sweep_dir;
            m_sweep_dir = (m_sweep_dir % 8) + 1;
        }
    }

    void process_radar_results(const std::vector<RadarObj>& results) override
    {
        m_has_target = false;
        int my_r, my_c;
        get_current_location(my_r, my_c);

        int best_dist = std::numeric_limits<int>::max();

        for (const auto& obj : results)
        {
            // Accept live robots (R) and also robot symbols that arena uses
            if (obj.m_type != '.' && obj.m_type != 'M' &&
                obj.m_type != 'F' && obj.m_type != 'P' && obj.m_type != 'X')
            {
                int dist = std::abs(obj.m_row - my_r) + std::abs(obj.m_col - my_c);
                if (dist < best_dist)
                {
                    best_dist    = dist;
                    m_target_r   = obj.m_row;
                    m_target_c   = obj.m_col;
                    m_has_target = true;
                }
            }
        }

        if (!m_has_target)
            m_turns_no_target++;
        else
            m_turns_no_target = 0;
    }

    bool get_shot_location(int& shot_row, int& shot_col) override
    {
        if (!m_has_target) return false;

        int my_r, my_c;
        get_current_location(my_r, my_c);

        int dr = std::abs(m_target_r - my_r);
        int dc = std::abs(m_target_c - my_c);

        if (dr <= 1 && dc <= 1 && (dr + dc > 0))
        {
            shot_row = m_target_r;
            shot_col = m_target_c;
            return true;
        }

        return false;
    }

    void get_move_direction(int& move_direction, int& move_distance) override
    {
        int my_r, my_c;
        get_current_location(my_r, my_c);

        if (!m_has_target)
        {
            // Every 8 turns with no target, pick a completely new random direction
            // to avoid getting stuck in loops against walls/mounds
            if (m_turns_no_target % 8 == 0)
            {
                move_direction = (std::rand() % 8) + 1;
            }
            else
            {
                // Move toward center
                int center_r = m_board_row_max / 2;
                int center_c = m_board_col_max / 2;

                int best_dir   = 1;
                int best_score = std::numeric_limits<int>::max();
                for (int d = 1; d <= 8; ++d)
                {
                    int nr = my_r + directions[d].first;
                    int nc = my_c + directions[d].second;
                    int score = std::abs(center_r - nr) + std::abs(center_c - nc);
                    if (score < best_score)
                    {
                        best_score = score;
                        best_dir   = d;
                    }
                }
                move_direction = best_dir;
            }
            move_distance = get_move_speed();
            return;
        }

        // Chase target
        int best_dir   = 1;
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
        move_distance  = get_move_speed();
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
