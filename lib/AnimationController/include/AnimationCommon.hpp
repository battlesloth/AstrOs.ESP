#ifndef ANIMATIONCOMMON_H
#define ANIMATIONCOMMON_H

#include <string>
#include <vector>

typedef std::vector<std::string> str_vec_t;



typedef enum
{
    START,
    HOME,
    SPEED,
    POSITION,
    SPEED_INCREMENTAL,
    POSITION_INCREMENTAL
} KangarooAction;

#endif