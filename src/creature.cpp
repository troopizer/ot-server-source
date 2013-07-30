//////////////////////////////////////////////////////////////////////
// The Forgotten Server - a server application for the MMORPG Tibia
//////////////////////////////////////////////////////////////////////
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software Foundation,
// Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
//////////////////////////////////////////////////////////////////////

#include "otpch.h"

#include "creature.h"
#include "game.h"
#include "player.h"
#include "npc.h"
#include "monster.h"
#include "container.h"
#include "condition.h"
#include "combat.h"
#include "configmanager.h"

#include <string>
#include <vector>
#include <algorithm>

double Creature::speedA = 857.36;
double Creature::speedB = 261.29;
double Creature::speedC = -4795.01;

extern Game g_game;
extern ConfigManager g_config;
extern CreatureEvents* g_creatureEvents;

Creature::Creature() :
	isInternalRemoved(false)
{
	id = 0;
	_tile = NULL;
	direction = SOUTH;
	master = NULL;
	lootDrop = true;
	skillLoss = true;

	health = 1000;
	healthMax = 1000;
	mana = 0;
	manaMax = 0;

	lastStep = 0;
	lastStepCost = 1;
	baseSpeed = 220;
	varSpeed = 0;

	followCreature = NULL;
	hasFollowPath = false;
	eventWalk = 0;
	cancelNextWalk = false;
	forceUpdateFollowPath = false;
	isMapLoaded = false;
	isUpdatingPath = false;
	memset(localMapCache, 0, sizeof(localMapCache));

	attackedCreature = NULL;
	_lastHitCreature = NULL;
	_mostDamageCreature = NULL;
	lastHitUnjustified = false;
	mostDamageUnjustified = false;

	lastHitCreature = 0;
	blockCount = 0;
	blockTicks = 0;
	walkUpdateTicks = 0;
	checkCreatureVectorIndex = -1;
	creatureCheck = false;
	scriptEventsBitField = 0;

	hiddenHealth = false;

	onIdleStatus();
}

Creature::~Creature()
{
	std::list<Creature*>::iterator cit;

	for (cit = summons.begin(); cit != summons.end(); ++cit) {
		(*cit)->setAttackedCreature(NULL);
		(*cit)->setMaster(NULL);
		(*cit)->releaseThing2();
	}

	summons.clear();

	for (ConditionList::iterator it = conditions.begin(); it != conditions.end(); ++it) {
		(*it)->endCondition(this, CONDITIONEND_CLEANUP);
		delete *it;
	}

	conditions.clear();

	attackedCreature = NULL;

	//std::cout << "Creature destructor " << this->getID() << std::endl;
}

bool Creature::canSee(const Position& myPos, const Position& pos, int32_t viewRangeX, int32_t viewRangeY)
{
	if (myPos.z <= 7) {
		//we are on ground level or above (7 -> 0)
		//view is from 7 -> 0
		if (pos.z > 7) {
			return false;
		}
	} else if (myPos.z >= 8) {
		//we are underground (8 -> 15)
		//view is +/- 2 from the floor we stand on
		if (Position::getDistanceZ(myPos, pos) > 2) {
			return false;
		}
	}

	const int_fast32_t offsetz = myPos.getZ() - pos.getZ();
	return (pos.getX() >= myPos.getX() - viewRangeX + offsetz) && (pos.getX() <= myPos.getX() + viewRangeX + offsetz)
		&& (pos.getY() >= myPos.getY() - viewRangeY + offsetz) && (pos.getY() <= myPos.getY() + viewRangeY + offsetz);
}

bool Creature::canSee(const Position& pos) const
{
	return canSee(getPosition(), pos, Map::maxViewportX, Map::maxViewportY);
}

bool Creature::canSeeCreature(const Creature* creature) const
{
	if (!canSeeInvisibility() && creature->isInvisible()) {
		return false;
	}

	return true;
}

int64_t Creature::getTimeSinceLastMove() const
{
	if (lastStep) {
		return OTSYS_TIME() - lastStep;
	}

	return 0x7FFFFFFFFFFFFFFFLL;
}

int32_t Creature::getWalkDelay(Direction dir) const
{
	if (lastStep == 0) {
		return 0;
	}

	int64_t ct = OTSYS_TIME();
	int64_t stepDuration = getStepDuration(dir);
	return stepDuration - (ct - lastStep);
}

int32_t Creature::getWalkDelay() const
{
	//Used for auto-walking
	if (lastStep == 0) {
		return 0;
	}

	int64_t ct = OTSYS_TIME();
	int64_t stepDuration = getStepDuration() * lastStepCost;
	return stepDuration - (ct - lastStep);
}

void Creature::onThink(uint32_t interval)
{
	if (!isMapLoaded && useCacheMap()) {
		isMapLoaded = true;
		updateMapCache();
	}

	if (followCreature && getMaster() != followCreature && !canSeeCreature(followCreature)) {
		onCreatureDisappear(followCreature, false);
	}

	if (attackedCreature && getMaster() != attackedCreature && !canSeeCreature(attackedCreature)) {
		onCreatureDisappear(attackedCreature, false);
	}

	blockTicks += interval;

	if (blockTicks >= 1000) {
		blockCount = std::min<uint32_t>(blockCount + 1, 2);
		blockTicks = 0;
	}

	if (followCreature) {
		walkUpdateTicks += interval;

		if (forceUpdateFollowPath || walkUpdateTicks >= 2000) {
			walkUpdateTicks = 0;
			forceUpdateFollowPath = false;
			isUpdatingPath = true;
		}
	}

	if (isUpdatingPath) {
		isUpdatingPath = false;
		goToFollowCreature();
	}

	//scripting event - onThink
	CreatureEventList thinkEvents = getCreatureEvents(CREATURE_EVENT_THINK);

	for (CreatureEventList::const_iterator it = thinkEvents.begin(), end = thinkEvents.end(); it != end; ++it) {
		(*it)->executeOnThink(this, interval);
	}
}

void Creature::onAttacking(uint32_t interval)
{
	if (!attackedCreature) {
		return;
	}

	onAttacked();
	attackedCreature->onAttacked();

	if (g_game.isSightClear(getPosition(), attackedCreature->getPosition(), true)) {
		doAttacking(interval);
	}
}

void Creature::onIdleStatus()
{
	if (getHealth() > 0) {
		healMap.clear();
		damageMap.clear();
	}
}

void Creature::onWalk()
{
	if (getWalkDelay() <= 0) {
		Direction dir;
		uint32_t flags = FLAG_IGNOREFIELDDAMAGE;

		if (getNextStep(dir, flags)) {
			ReturnValue ret = g_game.internalMoveCreature(this, dir, flags);

			if (ret != RET_NOERROR) {
				if (Player* player = getPlayer()) {
					player->sendCancelMessage(ret);
					player->sendCancelWalk();
				}

				forceUpdateFollowPath = true;
			}
		} else {
			if (listWalkDir.empty()) {
				onWalkComplete();
			}

			stopEventWalk();
		}
	}

	if (cancelNextWalk) {
		listWalkDir.clear();
		onWalkAborted();
		cancelNextWalk = false;
	}

	if (eventWalk != 0) {
		eventWalk = 0;
		addEventWalk();
	}
}

void Creature::onWalk(Direction& dir)
{
	if (hasCondition(CONDITION_DRUNK)) {
		uint32_t r = random_range(0, 20);

		if (r <= 4) {
			switch (r) {
				case 0:
					dir = NORTH;
					break;
				case 1:
					dir = WEST;
					break;
				case 3:
					dir = SOUTH;
					break;
				case 4:
					dir = EAST;
					break;

				default:
					break;
			}

			g_game.internalCreatureSay(this, SPEAK_MONSTER_SAY, "Hicks!", false);
		}
	}
}

bool Creature::getNextStep(Direction& dir, uint32_t& flags)
{
	if (listWalkDir.empty()) {
		return false;
	}

	dir = listWalkDir.front();
	listWalkDir.pop_front();
	onWalk(dir);
	return true;
}

bool Creature::startAutoWalk(std::list<Direction>& listDir)
{
	const Player* thisPlayer = getPlayer();

	if (thisPlayer && thisPlayer->getNoMove()) {
		thisPlayer->sendCancelWalk();
		return false;
	}

	listWalkDir = listDir;
	addEventWalk(listDir.size() == 1);
	return true;
}

void Creature::addEventWalk(bool firstStep)
{
	cancelNextWalk = false;

	if (getStepSpeed() <= 0) {
		return;
	}

	if (eventWalk != 0) {
		return;
	}

	int64_t ticks = getEventStepTicks(firstStep);

	if (ticks <= 0) {
		return;
	}

	// Take first step right away, but still queue the next
	if (ticks == 1) {
		g_game.checkCreatureWalk(getID());
	}

	eventWalk = g_scheduler.addEvent(createSchedulerTask(
	                                     std::max<int64_t>(SCHEDULER_MINTICKS, ticks), boost::bind(&Game::checkCreatureWalk, &g_game, getID())));
}

void Creature::stopEventWalk()
{
	if (eventWalk != 0) {
		g_scheduler.stopEvent(eventWalk);
		eventWalk = 0;
	}
}

void Creature::updateMapCache()
{
	Tile* tile;
	const Position& myPos = getPosition();
	Position pos(0, 0, myPos.z);

	for (int32_t y = -((mapWalkHeight - 1) / 2); y <= ((mapWalkHeight - 1) / 2); ++y) {
		for (int32_t x = -((mapWalkWidth - 1) / 2); x <= ((mapWalkWidth - 1) / 2); ++x) {
			pos.x = myPos.getX() + x;
			pos.y = myPos.getY() + y;
			tile = g_game.getTile(pos.x, pos.y, myPos.z);
			updateTileCache(tile, pos);
		}
	}
}

void Creature::updateTileCache(const Tile* tile, int32_t dx, int32_t dy)
{
	if ((std::abs(dx) <= (mapWalkWidth - 1) / 2) &&
	        (std::abs(dy) <= (mapWalkHeight - 1) / 2)) {
		int32_t x = (mapWalkWidth - 1) / 2 + dx;
		int32_t y = (mapWalkHeight - 1) / 2 + dy;

		localMapCache[y][x] = (tile && tile->__queryAdd(0, this, 1,
		                       FLAG_PATHFINDING | FLAG_IGNOREFIELDDAMAGE) == RET_NOERROR);
	}
}

void Creature::updateTileCache(const Tile* tile, const Position& pos)
{
	const Position& myPos = getPosition();

	if (pos.z == myPos.z) {
		int32_t dx = Position::getOffsetX(pos, myPos);
		int32_t dy = Position::getOffsetY(pos, myPos);
		updateTileCache(tile, dx, dy);
	}
}

int32_t Creature::getWalkCache(const Position& pos) const
{
	if (!useCacheMap()) {
		return 2;
	}

	const Position& myPos = getPosition();

	if (myPos.z != pos.z) {
		return 0;
	}

	if (pos == myPos) {
		return 1;
	}

	int32_t dx = Position::getOffsetX(pos, myPos);
	int32_t dy = Position::getOffsetY(pos, myPos);

	if ((std::abs(dx) <= (mapWalkWidth - 1) / 2) &&
	        (std::abs(dy) <= (mapWalkHeight - 1) / 2)) {
		int32_t x = (mapWalkWidth - 1) / 2 + dx;
		int32_t y = (mapWalkHeight - 1) / 2 + dy;
		if (localMapCache[y][x]) {
			return 1;
		} else {
			return 0;
		}
	}

	//out of range
	return 2;
}

void Creature::onAddTileItem(const Tile* tile, const Position& pos, const Item* item)
{
	if (isMapLoaded && pos.z == getPosition().z) {
		updateTileCache(tile, pos);
	}
}

void Creature::onUpdateTileItem(const Tile* tile, const Position& pos, const Item* oldItem,
                                const ItemType& oldType, const Item* newItem, const ItemType& newType)
{
	if (!isMapLoaded) {
		return;
	}

	if (oldType.blockSolid || oldType.blockPathFind || newType.blockPathFind || newType.blockSolid) {
		if (pos.z == getPosition().z) {
			updateTileCache(tile, pos);
		}
	}
}

void Creature::onRemoveTileItem(const Tile* tile, const Position& pos, const ItemType& iType,
                                const Item* item)
{
	if (!isMapLoaded) {
		return;
	}

	if (iType.blockSolid || iType.blockPathFind || iType.isGroundTile()) {
		if (pos.z == getPosition().z) {
			updateTileCache(tile, pos);
		}
	}
}

void Creature::onCreatureAppear(const Creature* creature, bool isLogin)
{
	if (creature == this) {
		if (useCacheMap()) {
			isMapLoaded = true;
			updateMapCache();
		}

		if (isLogin) {
			setLastPosition(getPosition());
		}
	} else if (isMapLoaded) {
		if (creature->getPosition().z == getPosition().z) {
			updateTileCache(creature->getTile(), creature->getPosition());
		}
	}
}

void Creature::onCreatureDisappear(const Creature* creature, uint32_t stackpos, bool isLogout)
{
	onCreatureDisappear(creature, true);

	if (creature == this) {
		if (getMaster() && !getMaster()->isRemoved()) {
			getMaster()->removeSummon(this);
		}
	} else if (isMapLoaded) {
		if (creature->getPosition().z == getPosition().z) {
			updateTileCache(creature->getTile(), creature->getPosition());
		}
	}
}

void Creature::onCreatureDisappear(const Creature* creature, bool isLogout)
{
	if (attackedCreature == creature) {
		setAttackedCreature(NULL);
		onAttackedCreatureDisappear(isLogout);
	}

	if (followCreature == creature) {
		setFollowCreature(NULL);
		onFollowCreatureDisappear(isLogout);
	}
}

void Creature::onChangeZone(ZoneType_t zone)
{
	if (attackedCreature && zone == ZONE_PROTECTION) {
		onCreatureDisappear(attackedCreature, false);
	}
}

void Creature::onAttackedCreatureChangeZone(ZoneType_t zone)
{
	if (zone == ZONE_PROTECTION) {
		onCreatureDisappear(attackedCreature, false);
	}
}

void Creature::onCreatureMove(const Creature* creature, const Tile* newTile, const Position& newPos,
                              const Tile* oldTile, const Position& oldPos, bool teleport)
{
	if (creature == this) {
		lastStep = OTSYS_TIME();
		lastStepCost = 1;

		if (!teleport) {
			if (oldPos.z != newPos.z) {
				//floor change extra cost
				lastStepCost = 2;
			} else if (Position::getDistanceX(newPos, oldPos) >= 1 && Position::getDistanceY(newPos, oldPos) >= 1) {
				//diagonal extra cost
				lastStepCost = 2;
			}
		} else {
			stopEventWalk();
		}

		if (!summons.empty()) {
			//check if any of our summons is out of range (+/- 2 floors or 30 tiles away)
			std::list<Creature*> despawnList;
			std::list<Creature*>::iterator cit;

			for (cit = summons.begin(); cit != summons.end(); ++cit) {
				const Position pos = (*cit)->getPosition();
				if (Position::getDistanceZ(newPos, pos) > 2 || (std::max<int32_t>(Position::getDistanceX(newPos, pos), Position::getDistanceY(newPos, pos)) > 30)) {
					despawnList.push_back((*cit));
				}
			}

			for (cit = despawnList.begin(); cit != despawnList.end(); ++cit) {
				g_game.removeCreature((*cit), true);
			}
		}

		if (newTile->getZone() != oldTile->getZone()) {
			onChangeZone(getZone());
		}

		//update map cache
		if (isMapLoaded) {
			if (teleport || oldPos.z != newPos.z) {
				updateMapCache();
			} else {
				Tile* tile;
				const Position& myPos = getPosition();
				Position pos;

				if (oldPos.y > newPos.y) { //north
					//shift y south
					for (int32_t y = mapWalkHeight - 1 - 1; y >= 0; --y) {
						memcpy(localMapCache[y + 1], localMapCache[y], sizeof(localMapCache[y]));
					}

					//update 0
					for (int32_t x = -((mapWalkWidth - 1) / 2); x <= ((mapWalkWidth - 1) / 2); ++x) {
						tile = g_game.getTile(myPos.getX() + x, myPos.getY() - ((mapWalkHeight - 1) / 2), myPos.z);
						updateTileCache(tile, x, -((mapWalkHeight - 1) / 2));
					}
				} else if (oldPos.y < newPos.y) { // south
					//shift y north
					for (int32_t y = 0; y <= mapWalkHeight - 1 - 1; ++y) {
						memcpy(localMapCache[y], localMapCache[y + 1], sizeof(localMapCache[y]));
					}

					//update mapWalkHeight - 1
					for (int32_t x = -((mapWalkWidth - 1) / 2); x <= ((mapWalkWidth - 1) / 2); ++x) {
						tile = g_game.getTile(myPos.getX() + x, myPos.getY() + ((mapWalkHeight - 1) / 2), myPos.z);
						updateTileCache(tile, x, (mapWalkHeight - 1) / 2);
					}
				}

				if (oldPos.x < newPos.x) { // east
					//shift y west
					int32_t starty = 0;
					int32_t endy = mapWalkHeight - 1;
					int32_t dy = Position::getDistanceY(oldPos, newPos);

					if (dy < 0) {
						endy = endy + dy;
					} else if (dy > 0) {
						starty = starty + dy;
					}

					for (int32_t y = starty; y <= endy; ++y) {
						for (int32_t x = 0; x <= mapWalkWidth - 1 - 1; ++x) {
							localMapCache[y][x] = localMapCache[y][x + 1];
						}
					}

					//update mapWalkWidth - 1
					for (int32_t y = -((mapWalkHeight - 1) / 2); y <= ((mapWalkHeight - 1) / 2); ++y) {
						tile = g_game.getTile(myPos.x + ((mapWalkWidth - 1) / 2), myPos.y + y, myPos.z);
						updateTileCache(tile, (mapWalkWidth - 1) / 2, y);
					}
				} else if (oldPos.x > newPos.x) { // west
					//shift y east
					int32_t starty = 0;
					int32_t endy = mapWalkHeight - 1;
					int32_t dy = Position::getDistanceY(oldPos, newPos);

					if (dy < 0) {
						endy = endy + dy;
					} else if (dy > 0) {
						starty = starty + dy;
					}

					for (int32_t y = starty; y <= endy; ++y) {
						for (int32_t x = mapWalkWidth - 1 - 1; x >= 0; --x) {
							localMapCache[y][x + 1] = localMapCache[y][x];
						}
					}

					//update 0
					for (int32_t y = -((mapWalkHeight - 1) / 2); y <= ((mapWalkHeight - 1) / 2); ++y) {
						tile = g_game.getTile(myPos.x - ((mapWalkWidth - 1) / 2), myPos.y + y, myPos.z);
						updateTileCache(tile, -((mapWalkWidth - 1) / 2), y);
					}
				}

				updateTileCache(oldTile, oldPos);
			}
		}
	} else {
		if (isMapLoaded) {
			const Position& myPos = getPosition();

			if (newPos.z == myPos.z) {
				updateTileCache(newTile, newPos);
			}

			if (oldPos.z == myPos.z) {
				updateTileCache(oldTile, oldPos);
			}
		}
	}

	if (creature == followCreature || (creature == this && followCreature)) {
		if (hasFollowPath) {
			isUpdatingPath = true;
		}

		if (newPos.z != oldPos.z || !canSee(followCreature->getPosition())) {
			onCreatureDisappear(followCreature, false);
		}
	}

	if (creature == attackedCreature || (creature == this && attackedCreature)) {
		if (newPos.z != oldPos.z || !canSee(attackedCreature->getPosition())) {
			onCreatureDisappear(attackedCreature, false);
		} else {
			if (hasExtraSwing()) {
				//our target is moving lets see if we can get in hit
				g_dispatcher.addTask(createTask(
				                         boost::bind(&Game::checkCreatureAttack, &g_game, getID())));
			}

			if (newTile->getZone() != oldTile->getZone()) {
				onAttackedCreatureChangeZone(attackedCreature->getZone());
			}
		}
	}
}

void Creature::onDeath()
{
	Creature* mostDamageCreatureMaster = NULL;
	Creature* lastHitCreatureMaster = NULL;

	if (getKillers(&_lastHitCreature, &_mostDamageCreature)) {
		if (_lastHitCreature) {
			lastHitUnjustified = _lastHitCreature->onKilledCreature(this);
			lastHitCreatureMaster = _lastHitCreature->getMaster();
		}

		if (_mostDamageCreature) {
			mostDamageCreatureMaster = _mostDamageCreature->getMaster();
			bool isNotLastHitMaster = (_mostDamageCreature != lastHitCreatureMaster);
			bool isNotMostDamageMaster = (_lastHitCreature != mostDamageCreatureMaster);
			bool isNotSameMaster = lastHitCreatureMaster == NULL || (mostDamageCreatureMaster != lastHitCreatureMaster);

			if (_mostDamageCreature != _lastHitCreature && isNotLastHitMaster && isNotMostDamageMaster && isNotSameMaster) {
				mostDamageUnjustified = _mostDamageCreature->onKilledCreature(this, false);
			}
		}
	}

	for (CountMap::iterator it = damageMap.begin(), end = damageMap.end(); it != end; ++it) {
		if (Creature* attacker = g_game.getCreatureByID(it->first)) {
			attacker->onAttackedCreatureKilled(this);
		}
	}

	bool droppedCorpse = dropCorpse();
	death();

	if (getMaster()) {
		getMaster()->removeSummon(this);
	}

	if (droppedCorpse) {
		g_game.removeCreature(this, false);
	}
}

bool Creature::dropCorpse()
{
	if (!lootDrop && getMonster() && !(master && master->getPlayer())) {
		if (master) {
			//scripting event - onDeath
			CreatureEventList deathEvents = getCreatureEvents(CREATURE_EVENT_DEATH);

			for (CreatureEventList::const_iterator it = deathEvents.begin(); it != deathEvents.end(); ++it) {
				(*it)->executeOnDeath(this, NULL, _lastHitCreature, _mostDamageCreature, lastHitUnjustified, mostDamageUnjustified);
			}
		}

		g_game.addMagicEffect(getPosition(), NM_ME_POFF);
	} else {
		Item* splash = NULL;

		switch (getRace()) {
			case RACE_VENOM:
				splash = Item::CreateItem(ITEM_FULLSPLASH, FLUID_GREEN);
				break;

			case RACE_BLOOD:
				splash = Item::CreateItem(ITEM_FULLSPLASH, FLUID_BLOOD);
				break;

			default:
				break;
		}

		Tile* tile = getTile();

		if (splash) {
			g_game.internalAddItem(tile, splash, INDEX_WHEREEVER, FLAG_NOLIMIT);
			g_game.startDecay(splash);
		}

		Item* corpse = getCorpse();

		if (corpse) {
			g_game.internalAddItem(tile, corpse, INDEX_WHEREEVER, FLAG_NOLIMIT);
			g_game.startDecay(corpse);
		}

		//scripting event - onDeath
		CreatureEventList deathEvents = getCreatureEvents(CREATURE_EVENT_DEATH);

		for (CreatureEventList::const_iterator it = deathEvents.begin(); it != deathEvents.end(); ++it) {
			(*it)->executeOnDeath(this, corpse, _lastHitCreature, _mostDamageCreature, lastHitUnjustified, mostDamageUnjustified);
		}

		if (corpse) {
			dropLoot(corpse->getContainer());
		}
	}

	return true;
}

bool Creature::getKillers(Creature** _lastHitCreature, Creature** _mostDamageCreature)
{
	*_lastHitCreature = g_game.getCreatureByID(lastHitCreature);
	*_mostDamageCreature = NULL;

	int32_t mostDamage = 0;

	for (CountMap::const_iterator it = damageMap.begin(), end = damageMap.end(); it != end; ++it) {
		CountBlock_t cb = it->second;
		if ((cb.total > mostDamage && (OTSYS_TIME() - cb.ticks <= g_game.getInFightTicks()))) {
			Creature* creature = g_game.getCreatureByID(it->first);
			if (creature) {
				mostDamage = cb.total;
				*_mostDamageCreature = creature;
			}
		}
	}

	return (*_lastHitCreature || *_mostDamageCreature);
}

bool Creature::hasBeenAttacked(uint32_t attackerId)
{
	CountMap::iterator it = damageMap.find(attackerId);

	if (it != damageMap.end()) {
		return (OTSYS_TIME() - it->second.ticks <= g_game.getInFightTicks());
	}

	return false;
}

Item* Creature::getCorpse()
{
	Item* corpse = Item::CreateItem(getLookCorpse());
	return corpse;
}

void Creature::changeHealth(int32_t healthChange, bool sendHealthChange/* = true*/)
{
	int32_t oldHealth = health;

	if (healthChange > 0) {
		health += std::min<int32_t>(healthChange, getMaxHealth() - health);
	} else {
		health = std::max<int32_t>(0, health + healthChange);
	}

	if (sendHealthChange && oldHealth != health) {
		g_game.addCreatureHealth(this);
	}
}

void Creature::changeMana(int32_t manaChange)
{
	if (manaChange > 0) {
		mana += std::min<int32_t>(manaChange, getMaxMana() - mana);
	} else {
		mana = std::max<int32_t>(0, mana + manaChange);
	}
}

void Creature::drainHealth(Creature* attacker, CombatType_t combatType, int32_t damage)
{
	changeHealth(-damage, false);

	if (attacker) {
		attacker->onAttackedCreatureDrainHealth(this, damage);
	}
}

void Creature::drainMana(Creature* attacker, int32_t manaLoss)
{
	onAttacked();
	changeMana(-manaLoss);
}

BlockType_t Creature::blockHit(Creature* attacker, CombatType_t combatType, int32_t& damage,
                               bool checkDefense /* = false */, bool checkArmor /* = false */)
{
	BlockType_t blockType = BLOCK_NONE;

	if (isImmune(combatType)) {
		damage = 0;
		blockType = BLOCK_IMMUNITY;
	} else if (checkDefense || checkArmor) {
		bool hasDefense = false;

		if (blockCount > 0) {
			--blockCount;
			hasDefense = true;
		}

		if (checkDefense && hasDefense) {
			int32_t maxDefense = getDefense();
			int32_t minDefense = maxDefense / 2;
			damage -= random_range(minDefense, maxDefense);

			if (damage <= 0) {
				damage = 0;
				blockType = BLOCK_DEFENSE;
				checkArmor = false;
			}
		}

		if (checkArmor) {
			int32_t armorValue = getArmor();
			int32_t minArmorReduction = 0;
			int32_t maxArmorReduction = 0;

			if (armorValue > 1) {
				minArmorReduction = (int32_t)std::ceil(armorValue * 0.475);
				maxArmorReduction = (int32_t)std::ceil(((armorValue * 0.475) - 1) + minArmorReduction);
			} else if (armorValue == 1) {
				minArmorReduction = 1;
				maxArmorReduction = 1;
			}

			damage -= random_range(minArmorReduction, maxArmorReduction);

			if (damage <= 0) {
				damage = 0;
				blockType = BLOCK_ARMOR;
			}
		}

		if (hasDefense && blockType != BLOCK_NONE) {
			onBlockHit(blockType);
		}
	}

	if (attacker) {
		attacker->onAttackedCreature(this);
		attacker->onAttackedCreatureBlockHit(this, blockType);
	}

	onAttacked();
	return blockType;
}

bool Creature::setAttackedCreature(Creature* creature)
{
	if (creature) {
		const Position& creaturePos = creature->getPosition();

		if (creaturePos.z != getPosition().z || !canSee(creaturePos)) {
			attackedCreature = NULL;
			return false;
		}
	}

	attackedCreature = creature;

	if (attackedCreature) {
		onAttackedCreature(attackedCreature);
		attackedCreature->onAttacked();
	}

	for (std::list<Creature*>::iterator cit = summons.begin(), end = summons.end(); cit != end; ++cit) {
		(*cit)->setAttackedCreature(creature);
	}

	return true;
}

void Creature::getPathSearchParams(const Creature* creature, FindPathParams& fpp) const
{
	fpp.fullPathSearch = !hasFollowPath;
	fpp.clearSight = true;
	fpp.maxSearchDist = 12;
	fpp.minTargetDist = 1;
	fpp.maxTargetDist = 1;
}

void Creature::goToFollowCreature()
{
	if (followCreature) {
		FindPathParams fpp;
		getPathSearchParams(followCreature, fpp);

		Monster* monster = getMonster();

		if (monster && !monster->getMaster() && (monster->isFleeing() || fpp.maxTargetDist > 1)) {
			Direction dir = NODIR;

			if (monster->isFleeing()) {
				monster->getDistanceStep(followCreature->getPosition(), dir, true);
			} else { //maxTargetDist > 1
				if (!monster->getDistanceStep(followCreature->getPosition(), dir)) {
					// if we can't get anything then let the A* calculate
					if (g_game.getPathToEx(this, followCreature->getPosition(), listWalkDir, fpp)) {
						hasFollowPath = true;
						startAutoWalk(listWalkDir);
					} else {
						hasFollowPath = false;
					}

					return;
				}
			}

			if (dir != NODIR) {
				listWalkDir.clear();
				listWalkDir.push_back(dir);

				hasFollowPath = true;
				startAutoWalk(listWalkDir);
			}
		} else {
			if (g_game.getPathToEx(this, followCreature->getPosition(), listWalkDir, fpp)) {
				hasFollowPath = true;
				startAutoWalk(listWalkDir);
			} else {
				hasFollowPath = false;
			}
		}
	}

	onFollowCreatureComplete(followCreature);
}

bool Creature::setFollowCreature(Creature* creature, bool fullPathSearch /*= false*/)
{
	if (creature) {
		if (followCreature == creature) {
			return true;
		}

		const Position& creaturePos = creature->getPosition();

		if (creaturePos.z != getPosition().z || !canSee(creaturePos)) {
			followCreature = NULL;
			return false;
		}

		if (!listWalkDir.empty()) {
			listWalkDir.clear();
			onWalkAborted();
		}

		hasFollowPath = false;
		forceUpdateFollowPath = false;
		followCreature = creature;
		isUpdatingPath = true;
	} else {
		isUpdatingPath = false;
		followCreature = NULL;
	}

	onFollowCreature(creature);
	return true;
}

double Creature::getDamageRatio(Creature* attacker) const
{
	int32_t totalDamage = 0;
	int32_t attackerDamage = 0;

	CountBlock_t cb;

	for (CountMap::const_iterator it = damageMap.begin(), end = damageMap.end(); it != end; ++it) {
		cb = it->second;
		totalDamage += cb.total;

		if (it->first == attacker->getID()) {
			attackerDamage += cb.total;
		}
	}

	return ((double)attackerDamage / totalDamage);
}

uint64_t Creature::getGainedExperience(Creature* attacker) const
{
	return std::floor(getDamageRatio(attacker) * getLostExperience());
}

void Creature::addDamagePoints(Creature* attacker, int32_t damagePoints)
{
	if (damagePoints <= 0) {
		return;
	}

	uint32_t attackerId = (attacker ? attacker->getID() : 0);

	CountMap::iterator it = damageMap.find(attackerId);

	if (it == damageMap.end()) {
		CountBlock_t cb;
		cb.ticks = OTSYS_TIME();
		cb.total = damagePoints;
		damageMap[attackerId] = cb;
	} else {
		it->second.total += damagePoints;
		it->second.ticks = OTSYS_TIME();
	}

	lastHitCreature = attackerId;
}

void Creature::addHealPoints(Creature* caster, int32_t healthPoints)
{
	if (healthPoints <= 0) {
		return;
	}

	uint32_t casterId = (caster ? caster->getID() : 0);

	CountMap::iterator it = healMap.find(casterId);

	if (it == healMap.end()) {
		CountBlock_t cb;
		cb.ticks = OTSYS_TIME();
		cb.total = healthPoints;
		healMap[casterId] = cb;
	} else {
		it->second.total += healthPoints;
		it->second.ticks = OTSYS_TIME();
	}
}

void Creature::onAddCondition(ConditionType_t type)
{
	if (type == CONDITION_PARALYZE && hasCondition(CONDITION_HASTE)) {
		removeCondition(CONDITION_HASTE);
	} else if (type == CONDITION_HASTE && hasCondition(CONDITION_PARALYZE)) {
		removeCondition(CONDITION_PARALYZE);
	}
}

void Creature::onAddCombatCondition(ConditionType_t type)
{
	//
}

void Creature::onEndCondition(ConditionType_t type)
{
	//
}

void Creature::onTickCondition(ConditionType_t type, bool& bRemove)
{
	const MagicField* field = getTile()->getFieldItem();

	if (!field) {
		return;
	}

	switch (type) {
		case CONDITION_FIRE:
			bRemove = (field->getCombatType() != COMBAT_FIREDAMAGE);
			break;
		case CONDITION_ENERGY:
			bRemove = (field->getCombatType() != COMBAT_ENERGYDAMAGE);
			break;
		case CONDITION_POISON:
			bRemove = (field->getCombatType() != COMBAT_EARTHDAMAGE);
			break;
		case CONDITION_FREEZING:
			bRemove = (field->getCombatType() != COMBAT_ICEDAMAGE);
			break;
		case CONDITION_DAZZLED:
			bRemove = (field->getCombatType() != COMBAT_HOLYDAMAGE);
			break;
		case CONDITION_CURSED:
			bRemove = (field->getCombatType() != COMBAT_DEATHDAMAGE);
			break;
		case CONDITION_DROWN:
			bRemove = (field->getCombatType() != COMBAT_DROWNDAMAGE);
			break;
		case CONDITION_BLEEDING:
			bRemove = (field->getCombatType() != COMBAT_PHYSICALDAMAGE);
			break;
		default:
			break;
	}
}

void Creature::onCombatRemoveCondition(const Creature* attacker, Condition* condition)
{
	removeCondition(condition);
}

void Creature::onAttackedCreature(Creature* target)
{
	//
}

void Creature::onAttacked()
{
	//
}

void Creature::onAttackedCreatureDrainHealth(Creature* target, int32_t points)
{
	target->addDamagePoints(this, points);

	Creature* masterCreature = getMaster();

	if (!masterCreature) {
		return;
	}

	Player* masterPlayer = masterCreature->getPlayer();

	if (masterPlayer) {
		std::ostringstream ss;
		ss << "Your " << asLowerCaseString(getName()) << " deals " << points << " to " << target->getNameDescription() << ".";
		masterPlayer->sendTextMessage(MSG_EVENT_DEFAULT, ss.str());
	}
}

void Creature::onTargetCreatureGainHealth(Creature* target, int32_t points)
{
	target->addHealPoints(this, points);
}

void Creature::onAttackedCreatureKilled(Creature* target)
{
	if (target != this) {
		uint64_t gainExp = target->getGainedExperience(this);
		onGainExperience(gainExp, target);
	}
}

bool Creature::onKilledCreature(Creature* target, bool lastHit/* = true*/)
{
	if (getMaster()) {
		getMaster()->onKilledCreature(target);
	}

	//scripting event - onKill
	CreatureEventList killEvents = getCreatureEvents(CREATURE_EVENT_KILL);

	for (CreatureEventList::const_iterator it = killEvents.begin(), end = killEvents.end(); it != end; ++it) {
		(*it)->executeOnKill(this, target);
	}

	return false;
}

void Creature::onGainExperience(uint64_t gainExp, Creature* target)
{
	if (gainExp != 0 && getMaster()) {
		gainExp = gainExp / 2;
		getMaster()->onGainExperience(gainExp, target);

		const Position& targetPos = getPosition();

		std::ostringstream ssExp;
		ssExp << ucfirst(getNameDescription()) << " gained " << gainExp << " experience points.";
		std::string strExp = ssExp.str();

		SpectatorVec list;
		g_game.getSpectators(list, targetPos, false, true);

		for (SpectatorVec::const_iterator it = list.begin(), end = list.end(); it != end; ++it) {
			(*it)->getPlayer()->sendExperienceMessage(MSG_EXPERIENCE_OTHERS, strExp, targetPos, gainExp, TEXTCOLOR_WHITE_EXP);
		}
	}
}

void Creature::onGainSharedExperience(uint64_t gainExp)
{
	//
}

void Creature::onAttackedCreatureBlockHit(Creature* target, BlockType_t blockType)
{
	//
}

void Creature::onBlockHit(BlockType_t blockType)
{
	//
}

void Creature::addSummon(Creature* creature)
{
	//std::cout << "addSummon: " << this << " summon=" << creature << std::endl;
	creature->setDropLoot(false);
	creature->setLossSkill(false);
	creature->setMaster(this);
	creature->useThing2();
	summons.push_back(creature);
}

void Creature::removeSummon(const Creature* creature)
{
	//std::cout << "removeSummon: " << this << " summon=" << creature << std::endl;
	std::list<Creature*>::iterator cit = std::find(summons.begin(), summons.end(), creature);

	if (cit != summons.end()) {
		(*cit)->setDropLoot(false);
		(*cit)->setLossSkill(true);
		(*cit)->setMaster(NULL);
		(*cit)->releaseThing2();
		summons.erase(cit);
	}
}

bool Creature::addCondition(Condition* condition, bool force/* = false*/)
{
	if (condition == NULL) {
		return false;
	}

	if (!force && condition->getType() == CONDITION_HASTE && hasCondition(CONDITION_PARALYZE)) {
		int64_t walkDelay = getWalkDelay();

		if (walkDelay > 0) {
			g_scheduler.addEvent(createSchedulerTask(walkDelay, boost::bind(&Game::forceAddCondition, &g_game, getID(), condition)));
			return false;
		}
	}

	Condition* prevCond = getCondition(condition->getType(), condition->getId(), condition->getSubId());

	if (prevCond) {
		prevCond->addCondition(this, condition);
		delete condition;
		return true;
	}

	if (condition->startCondition(this)) {
		conditions.push_back(condition);
		onAddCondition(condition->getType());
		return true;
	}

	delete condition;
	return false;
}

bool Creature::addCombatCondition(Condition* condition)
{
	//Caution: condition variable could be deleted after the call to addCondition
	ConditionType_t type = condition->getType();

	if (!addCondition(condition)) {
		return false;
	}

	onAddCombatCondition(type);
	return true;
}

void Creature::removeCondition(ConditionType_t type, bool force/* = false*/)
{
	for (ConditionList::iterator it = conditions.begin(); it != conditions.end();) {
		if ((*it)->getType() != type) {
			++it;
			continue;
		}

		Condition* condition = *it;

		if (!force && (*it)->getType() == CONDITION_PARALYZE) {
			int64_t walkDelay = getWalkDelay();

			if (walkDelay > 0) {
				g_scheduler.addEvent(createSchedulerTask(walkDelay, boost::bind(&Game::forceRemoveCondition, &g_game, getID(), type)));
				return;
			}
		}

		it = conditions.erase(it);

		condition->endCondition(this, CONDITIONEND_ABORT);
		delete condition;

		onEndCondition(type);
	}
}

void Creature::removeCondition(ConditionType_t type, ConditionId_t id, bool force/* = false*/)
{
	for (ConditionList::iterator it = conditions.begin(); it != conditions.end();) {
		if ((*it)->getType() != type || (*it)->getId() != id) {
			++it;
			continue;
		}

		Condition* condition = *it;

		if (!force && (*it)->getType() == CONDITION_PARALYZE) {
			int64_t walkDelay = getWalkDelay();

			if (walkDelay > 0) {
				g_scheduler.addEvent(createSchedulerTask(walkDelay, boost::bind(&Game::forceRemoveCondition, &g_game, getID(), type)));
				return;
			}
		}

		it = conditions.erase(it);

		condition->endCondition(this, CONDITIONEND_ABORT);
		delete condition;

		onEndCondition(type);
	}
}

void Creature::removeCondition(const Creature* attacker, ConditionType_t type)
{
	ConditionList tmpList = conditions;

	for (ConditionList::iterator it = tmpList.begin(), end = tmpList.end(); it != end; ++it) {
		if ((*it)->getType() == type) {
			onCombatRemoveCondition(attacker, *it);
		}
	}
}

void Creature::removeCondition(Condition* condition, bool force/* = false*/)
{
	ConditionList::iterator it = std::find(conditions.begin(), conditions.end(), condition);

	if (it == conditions.end()) {
		return;
	}

	if (!force && condition->getType() == CONDITION_PARALYZE) {
		int64_t walkDelay = getWalkDelay();

		if (walkDelay > 0) {
			g_scheduler.addEvent(createSchedulerTask(walkDelay, boost::bind(&Game::forceRemoveCondition, &g_game, getID(), condition->getType())));
			return;
		}
	}

	conditions.erase(it);

	condition->endCondition(this, CONDITIONEND_ABORT);
	onEndCondition(condition->getType());
	delete condition;
}

Condition* Creature::getCondition(ConditionType_t type) const
{
	for (ConditionList::const_iterator it = conditions.begin(), end = conditions.end(); it != end; ++it) {
		if ((*it)->getType() == type) {
			return *it;
		}
	}

	return NULL;
}

Condition* Creature::getCondition(ConditionType_t type, ConditionId_t id, uint32_t subId/* = 0*/) const
{
	for (ConditionList::const_iterator it = conditions.begin(), end = conditions.end(); it != end; ++it) {
		if ((*it)->getType() == type && (*it)->getId() == id && (*it)->getSubId() == subId) {
			return *it;
		}
	}

	return NULL;
}

void Creature::executeConditions(uint32_t interval)
{
	for (ConditionList::iterator it = conditions.begin(); it != conditions.end();) {
		if (!(*it)->executeCondition(this, interval)) {
			ConditionType_t type = (*it)->getType();

			Condition* condition = *it;
			it = conditions.erase(it);

			condition->endCondition(this, CONDITIONEND_TICKS);
			delete condition;

			onEndCondition(type);
		} else {
			++it;
		}
	}
}

bool Creature::hasCondition(ConditionType_t type, uint32_t subId/* = 0*/) const
{
	if (type == CONDITION_EXHAUST_COMBAT && g_game.getStateTime() == 0) {
		return true;
	}

	if (isSuppress(type)) {
		return false;
	}

	for (ConditionList::const_iterator it = conditions.begin(), end = conditions.end(); it != end; ++it) {
		if ((*it)->getType() != type || (*it)->getSubId() != subId) {
			continue;
		}

		if (g_config.getBoolean(ConfigManager::OLD_CONDITION_ACCURACY)) {
			return true;
		}

		if ((*it)->getEndTime() == 0) {
			return true;
		}

		int64_t seekTime = g_game.getStateTime();

		if (seekTime == 0) {
			return true;
		}

		if ((*it)->getEndTime() >= seekTime) {
			seekTime = (*it)->getEndTime();
		}

		if (seekTime >= OTSYS_TIME()) {
			return true;
		}
	}

	return false;
}

bool Creature::isImmune(CombatType_t type) const
{
	return hasBitSet((uint32_t)type, getDamageImmunities());
}

bool Creature::isImmune(ConditionType_t type) const
{
	return hasBitSet((uint32_t)type, getConditionImmunities());
}

bool Creature::isSuppress(ConditionType_t type) const
{
	return hasBitSet((uint32_t)type, getConditionSuppressions());
}

std::string Creature::getDescription(int32_t lookDistance) const
{
	std::string str = "a creature";
	return str;
}

int32_t Creature::getStepDuration(Direction dir) const
{
	int32_t stepDuration = getStepDuration();

	if (dir == NORTHWEST || dir == NORTHEAST || dir == SOUTHWEST || dir == SOUTHEAST) {
		stepDuration *= 3;
	}

	return stepDuration;
}

int32_t Creature::getStepDuration() const
{
	if (isRemoved()) {
		return 0;
	}

	uint32_t calculatedStepSpeed;
	uint32_t groundSpeed;

	int32_t stepSpeed = getStepSpeed();

	if (stepSpeed > -Creature::speedB) {
		calculatedStepSpeed = floor((Creature::speedA * log((stepSpeed / 2) + Creature::speedB) + Creature::speedC) + 0.5);

		if (calculatedStepSpeed <= 0) {
			calculatedStepSpeed = 1;
		}
	} else {
		calculatedStepSpeed = 1;
	}

	const Tile* tile = getTile();

	if (tile && tile->ground) {
		uint32_t groundId = tile->ground->getID();
		groundSpeed = Item::items[groundId].speed;

		if (groundSpeed == 0) {
			groundSpeed = 150;
		}
	} else {
		groundSpeed = 150;
	}

	double duration = std::floor(1000 * groundSpeed / calculatedStepSpeed);

	int32_t stepDuration = std::ceil(duration / 50) * 50;

	const Monster* monster = getMonster();

	if (monster && monster->isTargetNearby() && !monster->isFleeing() && !monster->getMaster()) {
		stepDuration <<= 1;
	}

	return stepDuration;
}

int64_t Creature::getEventStepTicks(bool onlyDelay) const
{
	int64_t ret = getWalkDelay();

	if (ret <= 0) {
		int32_t stepDuration = getStepDuration();

		if (onlyDelay && stepDuration > 0) {
			ret = 1;
		} else {
			ret = stepDuration * lastStepCost;
		}
	}

	return ret;
}

void Creature::getCreatureLight(LightInfo& light) const
{
	light = internalLight;
}

void Creature::setNormalCreatureLight()
{
	internalLight.level = 0;
	internalLight.color = 0;
}

bool Creature::registerCreatureEvent(const std::string& name)
{
	CreatureEvent* event = g_creatureEvents->getEventByName(name);

	if (!event) {
		return false;
	}

	CreatureEventType_t type = event->getEventType();

	if (hasEventRegistered(type)) {
		//check for duplicates
		for (CreatureEventList::const_iterator it = eventsList.begin(); it != eventsList.end(); ++it) {
			if (*it == event) {
				return false;
			}
		}
	} else {
		//set the bit
		scriptEventsBitField = scriptEventsBitField | ((uint32_t)1 << type);
	}

	eventsList.push_back(event);
	return true;
}

CreatureEventList Creature::getCreatureEvents(CreatureEventType_t type)
{
	CreatureEventList tmpEventList;

	if (!hasEventRegistered(type)) {
		return tmpEventList;
	}

	for (CreatureEventList::const_iterator it = eventsList.begin(), end = eventsList.end(); it != end; ++it) {
		if ((*it)->getEventType() == type) {
			tmpEventList.push_back(*it);
		}
	}

	return tmpEventList;
}

FrozenPathingConditionCall::FrozenPathingConditionCall(const Position& _targetPos)
{
	targetPos = _targetPos;
}

bool FrozenPathingConditionCall::isInRange(const Position& startPos, const Position& testPos,
        const FindPathParams& fpp) const
{
	if (fpp.fullPathSearch) {
		if (testPos.x > targetPos.x + fpp.maxTargetDist) {
			return false;
		}

		if (testPos.x < targetPos.x - fpp.maxTargetDist) {
			return false;
		}

		if (testPos.y > targetPos.y + fpp.maxTargetDist) {
			return false;
		}

		if (testPos.y < targetPos.y - fpp.maxTargetDist) {
			return false;
		}
	} else {
		int_fast32_t dx = Position::getOffsetX(startPos, targetPos);

		int32_t dxMax = (dx >= 0 ? fpp.maxTargetDist : 0);
		if (testPos.x > targetPos.x + dxMax) {
			return false;
		}

		int32_t dxMin = (dx <= 0 ? fpp.maxTargetDist : 0);
		if (testPos.x < targetPos.x - dxMin) {
			return false;
		}

		int_fast32_t dy = Position::getOffsetY(startPos, targetPos);

		int32_t dyMax = (dy >= 0 ? fpp.maxTargetDist : 0);
		if (testPos.y > targetPos.y + dyMax) {
			return false;
		}

		int32_t dyMin = (dy <= 0 ? fpp.maxTargetDist : 0);
		if (testPos.y < targetPos.y - dyMin) {
			return false;
		}
	}
	return true;
}

bool FrozenPathingConditionCall::operator()(const Position& startPos, const Position& testPos,
        const FindPathParams& fpp, int32_t& bestMatchDist) const
{
	if (!isInRange(startPos, testPos, fpp)) {
		return false;
	}

	if (fpp.clearSight && !g_game.isSightClear(testPos, targetPos, true)) {
		return false;
	}

	int32_t testDist = std::max<int32_t>(Position::getDistanceX(targetPos, testPos), Position::getDistanceY(targetPos, testPos));
	if (fpp.maxTargetDist == 1) {
		if (testDist < fpp.minTargetDist || testDist > fpp.maxTargetDist) {
			return false;
		}

		return true;
	} else if (testDist <= fpp.maxTargetDist) {
		if (testDist < fpp.minTargetDist) {
			return false;
		}

		if (testDist == fpp.maxTargetDist) {
			bestMatchDist = 0;
			return true;
		} else if (testDist > bestMatchDist) {
			//not quite what we want, but the best so far
			bestMatchDist = testDist;
			return true;
		}
	}
	return false;
}
