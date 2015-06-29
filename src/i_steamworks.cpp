/*
** i_steamworks.cpp
**
**---------------------------------------------------------------------------
** Copyright 2015 Braden Obrzut
** All rights reserved.
**
** Redistribution and use in source and binary forms, with or without
** modification, are permitted provided that the following conditions
** are met:
**
** 1. Redistributions of source code must retain the above copyright
**    notice, this list of conditions and the following disclaimer.
** 2. Redistributions in binary form must reproduce the above copyright
**    notice, this list of conditions and the following disclaimer in the
**    documentation and/or other materials provided with the distribution.
** 3. The name of the author may not be used to endorse or promote products
**    derived from this software without specific prior written permission.
**
** THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
** IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
** OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
** IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
** INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
** NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
** DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
** THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
** (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
** THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
**---------------------------------------------------------------------------
**
** Optional SteamWorks integration logic.
**
*/

#include "i_steamworks.h"
#include "id_ca.h"
#include "g_mapinfo.h"
#include "wl_def.h"
#include "wl_game.h"
#include "zstring.h"

namespace SteamWorks {

enum
{
	ACHIEVEMENT_Theologian,
	ACHIEVEMENT_FinishLevel1,
	ACHIEVEMENT_FinishLevel2,
	ACHIEVEMENT_FinishLevel3,
	ACHIEVEMENT_FinishLevel4,
	ACHIEVEMENT_FinishLevel5,
	ACHIEVEMENT_FinishLevel6,
	ACHIEVEMENT_FinishSecret1,
	ACHIEVEMENT_FinishSecret2,
	ACHIEVEMENT_CompleteLevel1,
	ACHIEVEMENT_CompleteLevel2,
	ACHIEVEMENT_CompleteLevel3,
	ACHIEVEMENT_CompleteLevel4,
	ACHIEVEMENT_CompleteLevel5,
	ACHIEVEMENT_CompleteLevel6,
	ACHIEVEMENT_AllSecrets,
	ACHIEVEMENT_AllAnimals,
	ACHIEVEMENT_AllFruits,
	ACHIEVEMENT_CompleteGame,
	ACHIEVEMENT_ArcadeLevel1,
	ACHIEVEMENT_ArcadeLevel2,
	ACHIEVEMENT_ArcadeLevel3,
	ACHIEVEMENT_ArcadeLevel4,
	ACHIEVEMENT_ArcadeLevel5,
	ACHIEVEMENT_ArcadeLevel6,
	ACHIEVEMENT_ArcadeGame,
	ACHIEVEMENT_SpeedrunPar
};
static void AwardAchievement(int achievement);
static bool CheckAchievement(int achievement);
static void ProgressAchievement(int achievement, int progress);

static bool RecordsDirty = false;
struct StatBool
{
	bool value;

	operator bool() const { return value; }
	const StatBool &operator=(bool set)
	{
		RecordsDirty = true;
		value = set;
		return *this;
	}
};

static void PackBits(char* dest, const StatBool* input, unsigned int N)
{
	for(unsigned int i = 0;i < N;i += 8)
	{
		if(i + 8 <= N)
		{
			*dest++ = (char)(
				(BYTE(input[i]))|
				(BYTE(input[i+1])<<1)|
				(BYTE(input[i+2])<<2)|
				(BYTE(input[i+3])<<3)|
				(BYTE(input[i+4])<<4)|
				(BYTE(input[i+5])<<5)|
				(BYTE(input[i+6])<<6)|
				(BYTE(input[i+7])<<7));
		}
		else
		{
			*dest = (char)(input[i]);
			for(unsigned int j = i+1;j < N;++j)
				*dest = (char)(BYTE(*dest)|(BYTE(input[j])<<(j-i)));
		}
	}
}

static void UnpackBits(StatBool* dest, const char* input, unsigned int N)
{
	for(unsigned int i = 0;i < N;++i)
		(*dest++).value = (BYTE(input[i/8])>>(i&7))&1;
}

enum { STAT_Animals, STAT_Fruit, STAT_Secrets };
// Persistent data for tracking progress on certain achievements.
static struct AchievementRecords
{
	StatBool levelsFinished[30];
	StatBool levelsStats[3][30];
	StatBool levelsComplete[30]; // Requires all 3 of the above simultaneously
	StatBool levelsCompleteHard[30];
	StatBool levelsUnderPar[30];
	StatBool questionsAnswered[99];

	void Read(const char buffer[41])
	{
		UnpackBits(levelsFinished, buffer, 30);
		UnpackBits(levelsStats[0], buffer+4, 30*3);
		UnpackBits(levelsComplete, buffer+16, 30);
		UnpackBits(levelsCompleteHard, buffer+20, 30);
		UnpackBits(levelsUnderPar, buffer+24, 30);
		UnpackBits(questionsAnswered, buffer+28, 99);
	}

	void Serialize(char buffer[41]) const
	{
		PackBits(buffer, levelsFinished, 30);
		PackBits(buffer+4, levelsStats[0], 30*3);
		PackBits(buffer+16, levelsComplete, 30);
		PackBits(buffer+20, levelsCompleteHard, 30);
		PackBits(buffer+24, levelsUnderPar, 30);
		PackBits(buffer+28, questionsAnswered, 99);
	}
} Records;

static bool SteamActive = false;
static bool AchievementsEnabled = true;

void CheatsEnabled()
{
	AchievementsEnabled = false;
}

// Starting level number constants for each episode
static const int LevelTable[7] = { 0, 3, 7, 12, 17, 23, 30 };
static const int SegmentMask[6] = { 0x7, 0x78, 0xF80, 0x1F000, 0x7E0000, 0x3F800000 };
enum { MAP_Secret1 = 11, MAP_Secret2 = 29 };

static int GetEpisode(int level)
{
	if(level < 0 || level > 29)
		return -1;

	for(int i = 0;i < 5;++i)
	{
		if(level < LevelTable[i+1])
			return i;
	}
	return 5;
}

static bool CheckEpisode(StatBool data[30], int episode, bool ignoreSecrets=false)
{
	for(int i = LevelTable[episode];i < LevelTable[episode+1];++i)
	{
		if(!data[i])
		{
			if(!ignoreSecrets || (i != MAP_Secret1 && i != MAP_Secret2))
				return false;
		}
	}
	return true;
}

template<int N>
static bool CheckAll(StatBool data[N])
{
	for(unsigned int i = 0;i < N;++i)
	{
		if(!data[i])
			return false;
	}
	return true;
}

template<int N>
static unsigned int CountBits(StatBool data[N])
{
	unsigned int ret = 0;
	for(unsigned int i = 0;i < N;++i)
		ret += data[i] ? 1 : 0;
	return ret;
}

// In case of failure, certain achievements can imply certain stats. Restoring
// some of these have pretty debatable use, but for example the levelStats
// implied by episode completion can keep progress indicators accurate.
static void RecoverStats()
{
	for(int i = 0;i < 6;++i)
	{
		if(CheckAchievement(ACHIEVEMENT_FinishLevel1 + i))
		{
			for(int j = LevelTable[i];j < LevelTable[i+1];++j)
			{
				if(j == MAP_Secret1 || j == MAP_Secret2)
					continue;
				Records.levelsFinished[j] = true;
			}
		}

		if(CheckAchievement(ACHIEVEMENT_CompleteLevel1 + i))
		{
			for(int j = LevelTable[i];j < LevelTable[i+1];++j)
			{
				Records.levelsComplete[j] = true;
				Records.levelsStats[STAT_Animals][j] = Records.levelsStats[STAT_Fruit][j] = Records.levelsStats[STAT_Secrets][j] = true;
			}
		}
	}

	if(CheckAchievement(ACHIEVEMENT_FinishSecret1))
		Records.levelsFinished[MAP_Secret1] = true;
	if(CheckAchievement(ACHIEVEMENT_FinishSecret2))
		Records.levelsFinished[MAP_Secret2] = true;

	if(CheckAchievement(ACHIEVEMENT_CompleteGame))
	{
		for(int i = 0;i < 30;++i)
			Records.levelsCompleteHard[i] = true;
	}

	if(CheckAchievement(ACHIEVEMENT_SpeedrunPar))
	{
		for(int i = 0;i < 30;++i)
			Records.levelsUnderPar[i] = true;
	}
}

// Hooks

static bool SingleSegment = false; // Elegable for Arkcade mode achievements?
static int StartingLevel = -1;
static int SegmentCompleted = 0;

void GameLoaded()
{
	SingleSegment = false;
	StartingLevel = -1;
	SegmentCompleted = 0;
}

void NewGame()
{
	SingleSegment = true;
	StartingLevel = levelInfo->LevelNumber-1;
	SegmentCompleted = 0;
}

void LevelCompleted(FString next)
{
	if(!AchievementsEnabled)
		return; // Cheater!

	int curLevel = levelInfo->LevelNumber-1;
	if(curLevel < 0 || curLevel > 29)
		return;

	const LevelInfo &nextLevel = LevelInfo::Find(next);
	bool episodeTransition = nextLevel.Cluster != levelInfo->Cluster;
	bool hard = gamestate.difficulty->PlayerDamageFactor == FRACUNIT;

	// Check for basic finishing achievement.
	if(!Records.levelsFinished[curLevel])
	{
		bool episodeFinished = CheckEpisode(Records.levelsFinished, GetEpisode(curLevel), true);
		Records.levelsFinished[curLevel] = true;
		if(curLevel != MAP_Secret1 && curLevel != MAP_Secret2 &&
			(episodeFinished != CheckEpisode(Records.levelsFinished, GetEpisode(curLevel), true)))
			AwardAchievement(ACHIEVEMENT_FinishLevel1 + GetEpisode(curLevel));

		if(curLevel == MAP_Secret1)
			AwardAchievement(ACHIEVEMENT_FinishSecret1);
		else if(curLevel == MAP_Secret2)
			AwardAchievement(ACHIEVEMENT_FinishSecret2);
	}

	// Under par
	if(levelInfo->Par > (unsigned)(gamestate.TimeCount/TICRATE))
	{
		if(!Records.levelsUnderPar[curLevel])
		{
			Records.levelsUnderPar[curLevel] = true;
			ProgressAchievement(ACHIEVEMENT_SpeedrunPar, CountBits<30>(Records.levelsUnderPar));
			if(CheckAll<30>(Records.levelsUnderPar))
				AwardAchievement(ACHIEVEMENT_SpeedrunPar);
		}
	}

	// Check Ratios
	const bool allAnimals = gamestate.killcount >= gamestate.killtotal;
	const bool allFruits = gamestate.treasurecount >= gamestate.treasuretotal;
	const bool allSecrets = gamestate.secretcount >= gamestate.secrettotal;
	if(allAnimals && !Records.levelsStats[STAT_Animals][curLevel])
	{
		Records.levelsStats[STAT_Animals][curLevel] = true;
		ProgressAchievement(ACHIEVEMENT_AllAnimals, CountBits<30>(Records.levelsStats[STAT_Animals]));
		if(CheckAll<30>(Records.levelsStats[STAT_Animals]))
			AwardAchievement(ACHIEVEMENT_AllAnimals);
	}
	if(allFruits && !Records.levelsStats[STAT_Fruit][curLevel])
	{
		Records.levelsStats[STAT_Fruit][curLevel] = true;
		ProgressAchievement(ACHIEVEMENT_AllFruits, CountBits<30>(Records.levelsStats[STAT_Fruit]));
		if(CheckAll<30>(Records.levelsStats[STAT_Fruit]))
			AwardAchievement(ACHIEVEMENT_AllFruits);
	}
	if(allSecrets && !Records.levelsStats[STAT_Secrets][curLevel])
	{
		Records.levelsStats[STAT_Secrets][curLevel] = true;
		ProgressAchievement(ACHIEVEMENT_AllSecrets, CountBits<30>(Records.levelsStats[STAT_Secrets]));
		if(CheckAll<30>(Records.levelsStats[STAT_Secrets]))
			AwardAchievement(ACHIEVEMENT_AllSecrets);
	}

	if(allAnimals && allFruits && allSecrets)
	{
		SegmentCompleted |= 1<<curLevel;

		// Level complete
		if(!Records.levelsComplete[curLevel])
		{
			bool episodeFinished = CheckEpisode(Records.levelsComplete, GetEpisode(curLevel));
			Records.levelsComplete[curLevel] = true;
			if(episodeFinished != CheckEpisode(Records.levelsComplete, GetEpisode(curLevel)))
				AwardAchievement(ACHIEVEMENT_CompleteLevel1 + GetEpisode(curLevel));
		}

		// Complete on hard
		if(hard)
		{
			if(!Records.levelsCompleteHard[curLevel])
			{
				Records.levelsCompleteHard[curLevel] = true;
				ProgressAchievement(ACHIEVEMENT_CompleteGame, CountBits<30>(Records.levelsCompleteHard));
				if(CheckAll<30>(Records.levelsCompleteHard))
					AwardAchievement(ACHIEVEMENT_CompleteGame);
			}
		}

		// Arkade mode
		if(episodeTransition && StartingLevel != -1)
		{
			if((SegmentCompleted & SegmentMask[GetEpisode(curLevel)]) == SegmentMask[GetEpisode(curLevel)])
				AwardAchievement(ACHIEVEMENT_ArcadeLevel1 + GetEpisode(curLevel));

			if(hard && curLevel == 28 && StartingLevel == 0 && SegmentCompleted == 0x3FFFFFFF)
				AwardAchievement(ACHIEVEMENT_ArcadeGame);
		}
	}

	// Renable Arkade mode on an episode transition
	if(episodeTransition && StartingLevel == -1)
		StartingLevel = nextLevel.LevelNumber-1;
}

void QuestionAnswered(int num)
{
	if(!AchievementsEnabled)
		return; // Cheater!

	if(num < 0 || num > 98)
		return;

	// Very easy not eligible.
	if(gamestate.difficulty->QuizHints)
		return;

	if(!Records.questionsAnswered[num])
	{
		Records.questionsAnswered[num] = true;
		ProgressAchievement(ACHIEVEMENT_Theologian, CountBits<99>(Records.questionsAnswered));
		if(CheckAll<99>(Records.questionsAnswered))
			AwardAchievement(ACHIEVEMENT_Theologian);
	}
}

}

#ifndef NO_STEAMWORKS
#include "i_steamworks_impl.cpp"
#else
namespace SteamWorks {

static void AwardAchievement(int) {}
static bool CheckAchievement(bool) { return false; }
static void ProgressAchievement(int, int) {}

void AsyncTick() {}

void Init()
{
	Printf("SteamWorks: Disabled.\n");
}

void Reset() {}
void Shutdown() {}

}
#endif
