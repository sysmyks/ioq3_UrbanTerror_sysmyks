/*
 *
 *	Ultimate modding by pedrxd
 *
 */

#include "server.h"

int   playerStatsOffsets[vMAX][OFFSET_PS_MAX] =
{
		[v42023] = 	{ 9, 0, 3},
		[v43] = 	{ 9, 0, 3},
		[v431] = 	{ 9, 0, 3},
		[v432] = 	{ 9, 0, 3},
		[v433] =	{ 9, 0, 3},
		[v434] =	{ 0, 6, 1}
};

//Values are enumerated here -> gclientOffsetStructure
int   gclientOffsets[vMAX][OFFSET_GL_MAX] =
{
		[v42023] = 	{ 3820, 2828},
		[v43] = 	{ 4328, 3352},
		[v431] = 	{ 4328, 3352},
		[v432] = 	{ 4328, 3352},
		[v433] =	{ 4328, 3096},
		[v434] =	{ 4328, 3096}
};

//Values are enumerated here -> weaponOffsetStructure
int   weaponOffsets[vMAX][OFFSET_WP_MAX] =
{
		//version    0     1   2   3   4   5    6   7    8    9   10
		[v42023] = {8256, 28, 32, 48, 52, 172, 36, 232, 231,  8, 432},
		[v43] =    {8649, 31, 35, 51, 55, 175, 39, 235, 231, 11, 432},
		[v431] =   {4365, 31, 35, 51, 55, 175, 39, 235, 231, 11, 432},
		[v432] =   {4393, 31, 35, 51, 55, 175, 39, 235, 231, 11, 432},
		[v433] =   {4449, 31, 35, 51, 55, 175, 39, 235, 231, 11, 432},
		[v434] =   {4449, 31, 35, 51, 55, 175, 39, 235, 231, 11, 432}
};

