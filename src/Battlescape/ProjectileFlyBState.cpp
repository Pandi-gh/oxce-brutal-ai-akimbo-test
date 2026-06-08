/*
 * Copyright 2010-2016 OpenXcom Developers.
 *
 * This file is part of OpenXcom.
 *
 * OpenXcom is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * OpenXcom is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with OpenXcom.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <algorithm>
#include <sstream>
#include "ProjectileFlyBState.h"
#include "ExplosionBState.h"
#include "Projectile.h"
#include "TileEngine.h"
#include "Map.h"
#include "../Savegame/BattleUnit.h"
#include "../Savegame/BattleItem.h"
#include "../Savegame/SavedBattleGame.h"
#include "../Savegame/Tile.h"
#include "../Savegame/HitLog.h"
#include "../Mod/Mod.h"
#include "../Engine/Sound.h"
#include "../Engine/RNG.h"
#include "../Mod/Armor.h"
#include "../Mod/RuleItem.h"
#include "../Engine/Options.h"
#include "../Engine/Logger.h"
#include "AIModule.h"
#include "Camera.h"
#include "Explosion.h"
#include "BattlescapeState.h"
#include "../Savegame/BattleUnitStatistics.h"
#include "../fmath.h"

namespace OpenXcom
{

/**
 * Sets up an ProjectileFlyBState.
 */
ProjectileFlyBState::ProjectileFlyBState(BattlescapeGame *parent, BattleAction action, Position origin, int range) : BattleState(parent, action), _unit(0), _ammo(0), _ammoOp(0), _origin(origin), _originVoxel(-1,-1,-1), _projectileImpact(0), _range(range), _initialized(false), _targetFloor(false)
{
}

ProjectileFlyBState::ProjectileFlyBState(BattlescapeGame *parent, BattleAction action) : BattleState(parent, action), _unit(0), _ammo(0), _ammoOp(0), _origin(action.actor->getPosition()), _originVoxel(-1,-1,-1), _projectileImpact(0), _range(0), _initialized(false), _targetFloor(false)
{
}

/**
 * Deletes the ProjectileFlyBState.
 */
ProjectileFlyBState::~ProjectileFlyBState()
{
}

/**
 * Initializes the sequence:
 * - checks if the shot is valid,
 * - calculates the base accuracy.
 */
void ProjectileFlyBState::init()
{
	if (_initialized) return;
	_initialized = true;

	BattleItem *weapon = _action.weapon;

	if (!weapon) // can't shoot without weapon
	{
		_parent->popState();
		return;
	}

	if (!_parent->getSave()->getTile(_action.target)) // invalid target position
	{
		_parent->popState();
		return;
	}

	//test TU only on first lunch waypoint or normal shoot
	if (_range == 0 && !_action.haveTU(&_action.result))
	{
		_parent->popState();
		return;
	}

	_unit = _action.actor;

	bool reactionShoot = _unit->getFaction() != _parent->getSave()->getSide();
	if (_action.type != BA_THROW)
	{
		_ammo = _action.weapon->getAmmoForAction(_action.type, reactionShoot ? nullptr : &_action.result);
		if (!_ammo)
		{
			_parent->popState();
			return;
		}
	}

	if (_unit->isOut() || _unit->isOutThresholdExceed())
	{
		// something went wrong - we can't shoot when dead or unconscious, or if we're about to fall over.
		_parent->popState();
		return;
	}

	// reaction fire
	if (reactionShoot)
	{
		auto target = _parent->getSave()->getTile(_action.target)->getUnit();
		// target is dead: cancel the shot.
		if (!target || target->isOut() || target->isOutThresholdExceed() || target != _parent->getSave()->getSelectedUnit())
		{
			_parent->popState();
			return;
		}
		_unit->lookAt(_action.target, _unit->getTurretType() != -1);
		while (_unit->getStatus() == STATUS_TURNING)
		{
			_unit->turn(_unit->getTurretType() != -1);
		}
	}

	Tile *endTile = _parent->getSave()->getTile(_action.target);
	int distanceSq = _action.actor->distance3dToPositionSq(_action.target);
	bool isPlayer = _parent->getSave()->getSide() == FACTION_PLAYER;
	if (isPlayer) _parent->getMap()->resetObstacles();
	switch (_action.type)
	{
	case BA_SNAPSHOT:
	case BA_AIMEDSHOT:
	case BA_AUTOSHOT:
	case BA_LAUNCH:
		if (weapon->getRules()->isOutOfRange(distanceSq))
		{
			// out of range
			_action.result = "STR_OUT_OF_RANGE";
			_parent->popState();
			return;
		}
		break;
	case BA_THROW:
		if (!validThrowRange(&_action, _parent->getTileEngine()->getOriginVoxel(_action, 0), _parent->getSave()->getTile(_action.target), _parent->getSave()->getDepth()))
		{
			// out of range
			_action.result = "STR_OUT_OF_RANGE";
			_parent->popState();
			return;
		}
		if (endTile &&
			endTile->getTerrainLevel() == -24 &&
			endTile->getPosition().z + 1 < _parent->getSave()->getMapSizeZ())
		{
			_action.target.z += 1;
		}
		break;
	case BA_AKIMBOSHOT:
		if (_unit->isAkimbo())
		{	
			if (_unit->getLeftHandWeapon()->getRules()->isOutOfRange(distanceSq) || _unit->getRightHandWeapon()->getRules()->isOutOfRange(distanceSq))
			{
				// out of range of any weapon in hands
				_action.result = "STR_OUT_OF_RANGE";
				_parent->popState();
				return;
			}
			// Align Active Hand = Main Hand for proper weapon switching process during AI activity, reaction shot and berserk state
			if (weapon != _unit->getActiveHand(_unit->getLeftHandWeapon(), _unit->getRightHandWeapon()))
			{
				if (_unit->getActiveHand(_unit->getLeftHandWeapon(), _unit->getRightHandWeapon()) == _unit->getLeftHandWeapon())
				{
					_unit->setActiveRightHand();
				}
				else
				{
					_unit->setActiveLeftHand();
				}
			}
			_ammo = weapon->getAmmoForAction(_action.type, reactionShoot ? nullptr : &_action.result);
			_ammoOp = _unit->getOppositeHandWeapon()->getAmmoForAction(_action.type, reactionShoot ? nullptr : &_action.result);
			_action.actWeaponShotQnty = weapon->getActionConf(BA_AKIMBOSHOT)->shots;
			_action.opWeaponShotQnty = _unit->getOppositeHandWeapon()->getActionConf(BA_AKIMBOSHOT)->shots;
			// if either weapon is out of ammo, there is no point to arrange akimbo
			if (!_ammo || !_ammoOp)
			{
				_action.result = "STR_NO_ROUNDS_LEFT";
				_parent->popState();
				return;
			}
		}
		break;
	default:
		_parent->popState();
		return;
	}

	// Check for close quarters combat
	if (_parent->getMod()->getEnableCloseQuartersCombat() && _action.type != BA_THROW && _action.type != BA_LAUNCH && _unit->getTurretType() == -1 && !_unit->getArmor()->getIgnoresMeleeThreat())
	{
		// Start by finding 'targets' for the check
		std::vector<BattleUnit*> closeQuartersTargetList;
		int surroundingTilePositions [8][2] = {
			{0, -1}, // north (-y direction)
			{1, -1}, // northeast
			{1, 0}, // east (+ x direction)
			{1, 1}, // southeast
			{0, 1}, // south (+y direction)
			{-1, 1}, // southwest
			{-1, 0}, // west (-x direction)
			{-1, -1}}; // northwest
		for (int dir = 0; dir < 8; dir++)
		{
			Position tileToCheck = _origin;
			tileToCheck.x += surroundingTilePositions[dir][0];
			tileToCheck.y += surroundingTilePositions[dir][1];

			if (_parent->getSave()->getTile(tileToCheck)) // Make sure the tile is in bounds
			{
				BattleUnit* closeQuartersTarget = _parent->getSave()->selectUnit(tileToCheck);
				// Variable for LOS check
				int checkDirection = _parent->getTileEngine()->getDirectionTo(tileToCheck, _unit->getPosition());
				if (closeQuartersTarget && _unit->getFaction() != closeQuartersTarget->getFaction() // Unit must exist and not be same faction
					&& closeQuartersTarget->getArmor()->getCreatesMeleeThreat() // Unit must be valid defender, 2x2 default false here
					&& closeQuartersTarget->getTimeUnits() >= _parent->getMod()->getCloseQuartersTuCostGlobal() // Unit must have enough TUs
					&& closeQuartersTarget->getEnergy() >= _parent->getMod()->getCloseQuartersEnergyCostGlobal() // Unit must have enough Energy
					&& _parent->getTileEngine()->validMeleeRange(closeQuartersTarget, _unit, checkDirection) // Unit must be able to see the unit attempting to fire
					&& !(_unit->getFaction() == FACTION_PLAYER && closeQuartersTarget->getFaction() == FACTION_NEUTRAL) // Civilians don't inhibit player
					&& !(_unit->getFaction() == FACTION_NEUTRAL && closeQuartersTarget->getFaction() == FACTION_PLAYER)) // Player doesn't inhibit civilians
				{
					if (RNG::percent(_parent->getMod()->getCloseQuartersSneakUpGlobal()))
					{
						if (_unit->getFaction() == FACTION_HOSTILE) // alien attacker (including mind-controlled xcom)
						{
							if (!closeQuartersTarget->hasVisibleUnit(_unit))
							{
								continue; // the xcom/civilian victim *DOES NOT SEE* the attacker and cannot defend itself
							}
						}
						else // xcom/civilian attacker (including mind-controlled aliens)
						{
							if (_unit->getTurnsSinceSpotted() > 1)
							{
								continue; // the aliens (as a collective) *ARE NOT AWARE* of the attacker and cannot defend themselves
							}
						}
					}
					closeQuartersTargetList.push_back(closeQuartersTarget);
				}
			}
		}

		if (!closeQuartersTargetList.empty())
		{
			int closeQuartersFailedResults[6] = {
				0,   // Fire straight down
				0,   // Fire straight up
				6,   // Fire left 90 degrees
				7,   // Fire left 45 degrees
				1,   // Fire right 45 degrees
				2 }; // Fire right 90 degrees

			for (auto* bu : closeQuartersTargetList)
			{
				BattleActionAttack attack;
				attack.type = BA_CQB;
				attack.attacker = _action.actor;
				attack.weapon_item = _action.weapon;
				attack.damage_item = _action.weapon;

				// Roll for the check
				if (!_parent->getTileEngine()->meleeAttack(attack, bu))
				{
					// Failed the check, roll again to see result
					if (_parent->getSave()->getSide() == FACTION_PLAYER) // Only show message during player's turn
					{
						_action.result = "STR_FAILED_CQB_CHECK";
					}
					int rng = RNG::generate(0, 5);
					Position closeQuartersFailedNewTarget = _unit->getPosition();
					if (rng == 1)
					{
						closeQuartersFailedNewTarget.z += 1;
					}
					else if (rng > 1)
					{
						int newFacing = (_unit->getDirection() + closeQuartersFailedResults[rng]) % 8;
						closeQuartersFailedNewTarget.x += surroundingTilePositions[newFacing][0];
						closeQuartersFailedNewTarget.y += surroundingTilePositions[newFacing][1];
					}

					// Make sure the new target is in bounds
					if (!_parent->getSave()->getTile(closeQuartersFailedNewTarget))
					{
						// Default to firing at our feet
						closeQuartersFailedNewTarget = _unit->getPosition();
					}

					// Turn to look at new target
					_action.target = closeQuartersFailedNewTarget;
					_unit->lookAt(_action.target, _unit->getTurretType() != -1);
					while (_unit->getStatus() == STATUS_TURNING)
					{
						_unit->turn(_unit->getTurretType() != -1);
					}

					// We're done, spend TUs and Energy; and don't check remaining CQB candidates anymore
					bu->spendTimeUnits(_parent->getMod()->getCloseQuartersTuCostGlobal());
					bu->spendEnergy(_parent->getMod()->getCloseQuartersEnergyCostGlobal());
					break;
				}
			}
		}
	}

	bool forceEnableObstacles = false;
	if (_action.type == BA_LAUNCH || (Options::forceFire && _parent->getSave()->isCtrlPressed(true) && isPlayer) || !_parent->getPanicHandled())
	{
		// target nothing, targets the middle of the tile
		_targetVoxel = _action.target.toVoxel() + TileEngine::voxelTileCenter;

        _originVoxel = _parent->getTileEngine()->getOriginVoxel(_action, _parent->getSave()->getTile(_origin));

		if (_action.type == BA_LAUNCH)
		{
			if (_targetFloor)
			{
				// launched missiles with two waypoints placed on the same tile: target the floor.
				_targetVoxel.z -= 10;
			}
			else
			{
				// launched missiles go slightly higher than the middle.
				_targetVoxel.z += 4;
			}
		}
	}
	else if (!_action.weapon->getArcingShot(_action.type))
	{
		// determine the target voxel.
		// aim at the center of the unit, the object, the walls or the floor (in that priority)
		// if there is no LOF to the center, try elsewhere (more outward).
		// Store this target voxel.
		Tile *targetTile = _parent->getSave()->getTile(_action.target);
		Position originVoxel = _parent->getTileEngine()->getOriginVoxel(_action, _parent->getSave()->getTile(_origin));
		bool foundLoF = false;
        bool selfShot = false;

		if (targetTile->getUnit() &&
			((_unit->getFaction() != FACTION_PLAYER) ||
			targetTile->getUnit()->getVisible()))
		{
			if (_origin == _action.target || targetTile->getUnit() == _unit)
			{
				// don't shoot at yourself but shoot at the floor
				_targetVoxel = _action.target.toVoxel() + Position(8, 8, 0);
                selfShot = true;
			}
			else if (Options::battleRealisticAccuracy)
			{
				std::vector<Position> exposedVoxels;
				OpenXcom::BattleActionOrigin bestOriginType;
				Position bestTargetPos;
				size_t bestExposedCount = 0;

				_parent->getTileEngine()->checkVoxelExposure(&originVoxel, targetTile, _unit, isPlayer, &exposedVoxels, nullptr, !isPlayer);

				if (!exposedVoxels.empty())
				{
					foundLoF = true;
					bestExposedCount = exposedVoxels.size();
					bestOriginType = BattleActionOrigin::CENTRE;
					bestTargetPos = exposedVoxels.at(0);
				}

				if (Options::oxceEnableOffCentreShooting) // Determine which shooting position is the best
				{
					for (auto& rel_pos : { BattleActionOrigin::LEFT, BattleActionOrigin::RIGHT })
					{
						exposedVoxels.clear();
						_action.relativeOrigin = rel_pos;
						originVoxel = _parent->getTileEngine()->getOriginVoxel(_action, _parent->getSave()->getTile(_origin));
						_parent->getTileEngine()->checkVoxelExposure(&originVoxel, targetTile, _unit, isPlayer, &exposedVoxels, nullptr, !isPlayer);

						if (exposedVoxels.size() <=  bestExposedCount) continue;

						foundLoF = true;
						bestExposedCount = exposedVoxels.size();
						bestOriginType = rel_pos;
						bestTargetPos = exposedVoxels.at(0);
					}
				}

				if (foundLoF) // Store the results
				{
					_targetVoxel = bestTargetPos;
					_action.relativeOrigin = bestOriginType;
				}
			}
			else // Classic Accuracy
			{
				foundLoF = _parent->getTileEngine()->canTargetUnit(&originVoxel, targetTile, &_targetVoxel, _unit, isPlayer);

				if (!foundLoF && Options::oxceEnableOffCentreShooting)
				{
					// If we can't target from the standard shooting position, try a bit left and right from the centre.
					for (auto& rel_pos : { BattleActionOrigin::LEFT, BattleActionOrigin::RIGHT })
					{
						_action.relativeOrigin = rel_pos;
						originVoxel = _parent->getTileEngine()->getOriginVoxel(_action, _parent->getSave()->getTile(_origin));
						foundLoF = _parent->getTileEngine()->canTargetUnit(&originVoxel, targetTile, &_targetVoxel, _unit, isPlayer);
						if (foundLoF)
						{
							break;
						}
					}
				}
			}

            if (!foundLoF && !selfShot)
			{
				// Failed to find LOF
				_action.relativeOrigin = BattleActionOrigin::CENTRE; // reset to the normal origin

				_targetVoxel = TileEngine::invalid.toVoxel(); // out of bounds, even after voxel to tile calculation.
				if (isPlayer)
				{
					forceEnableObstacles = true;
				}
			}
		}
		else
		{
			_targetVoxel = _parent->getTileEngine()->adjustTargetVoxelFromTileType(&originVoxel, targetTile, _unit, isPlayer);
		}
	}

	if (createNewProjectile())
	{
		auto conf = weapon->getActionConf(_action.type);
		if (_parent->getMap()->isAltPressed() || (conf && !conf->followProjectiles))
		{
			// temporarily turn off camera following projectiles to prevent annoying flashing effects (e.g. on minigun-like weapons)
			_parent->getMap()->setFollowProjectile(false);
		}
		if (_range == 0) _action.spendTU();
		_parent->getMap()->setCursorType(CT_NONE);
		_parent->getMap()->getCamera()->stopMouseScrolling();
		_parent->getMap()->disableObstacles();
		_unit->updateEnemyKnowledge(_parent->getSave()->getTileIndex(_unit->getPosition()), true);
	}
	else if (isPlayer && (_targetVoxel.z >= 0 || forceEnableObstacles))
	{
		_parent->getMap()->enableObstacles();
	}
}

/**
 * Tries to create a projectile sprite and add it to the map,
 * calculating its trajectory.
 * @return True, if the projectile was successfully created.
 */
bool ProjectileFlyBState::createNewProjectile()
{
	++_action.autoShotCounter;

	/*********************\ 
	* AKIMBO SHOTS SECTION *
	\*********************/
	if ( _action.type == BA_AKIMBOSHOT )
	{	// Remember original Active Hand weapon and ammo for hand iteration mechanism (ammo address need for projectile and impact "alignment")
		BattleItem *originWeapon = const_cast<BattleItem*>(_unit->getActiveHand(_unit->getLeftHandWeapon(), _unit->getRightHandWeapon()));
		BattleItem* originAmmo = originWeapon ? originWeapon->getAmmoForAction(_action.type, _unit->getFaction() != _parent->getSave()->getSide() ? nullptr : &_action.result) : 0;

		// Make possible remained shots (if it supposes) when weapon dissapeared and inactive hand asignes as active hand due ActiveHand result
		if (originWeapon && originAmmo && !_unit->getOppositeHandWeapon() && _action.opWeaponCounter < _action.actWeaponShotQnty)
		{
			_action.actWeaponCounter = _action.opWeaponCounter;
		}
		// Prevent switching to origin hand, if it has no weapon / no ammo
		if (!originWeapon || !originAmmo || originAmmo->getAmmoQuantity() == 0)
		{
			_action.actWeaponCounter = _action.actWeaponShotQnty;
		}
		// Prevent switching to opposite hand, if it has no weapon / no ammo
		if (!_unit->getOppositeHandWeapon() || !_ammoOp || _ammoOp->getAmmoQuantity() == 0)
		{
			_action.opWeaponCounter = _action.opWeaponShotQnty;
		}
		// All shots are done or impossible - stop shooting
		if (_action.actWeaponCounter >= _action.actWeaponShotQnty &&
			_action.opWeaponCounter >= _action.opWeaponShotQnty)
		{
			return false;
		}
		// Hand switch mechanic of proper weapon (and ammo) usage each shot, if everything fine
		if (_action.actWeaponCounter < _action.actWeaponShotQnty &&
			(_action.actWeaponCounter == _action.opWeaponCounter || _action.opWeaponCounter >= _action.opWeaponShotQnty))
		{
			++_action.actWeaponCounter;
			_action.weapon = originWeapon;
			_ammo = originAmmo;
			_action.updateTU();
		}
		else if (_action.opWeaponCounter < _action.opWeaponShotQnty &&
				 (_action.actWeaponCounter > _action.opWeaponCounter || _action.actWeaponCounter >= _action.actWeaponShotQnty))
		{
			++_action.opWeaponCounter;
			_action.weapon = _unit->getOppositeHandWeapon();
			_ammo = _ammoOp;
			_action.updateTU();
		}
	}

	// pWWWa/test: pierce bullet — initial damage of THIS particular projectile.
	// Conceptually piercePower is no longer a separate "pierce capacity" of the bullet;
	// it is the CURRENT damage carried by the bullet. The base damage is rolled by
	// RuleDamageType::getRandomDamage() ONCE here, at the muzzle, exactly as described:
	//     "пуля вылетает из ствола имея базовый урон + бонус за статы, затем умножается
	//      на модификатор полученный от броска кубика заданного для конкретной обоймы".
	// As the bullet flies through obstacles, this value is reduced by finalDecr each
	// terrain hit (Excel-formula); when it drops to <=0, Projectile::move() stops it.
	if (_ammo && _ammo->getRules()->getPierceType() && !(_action.type == BA_LAUNCH && _action.actor->getPosition() != _origin ))
	{
		int basePower = 0;
		if (_ammo->getRules()->getPiercePowerCap())
		{
			basePower = _ammo->getRules()->getPiercePowerCap();
		}
		else
		{
			basePower = _action.weapon && _action.weapon->getRules()->getIgnoreAmmoPower()
				? _action.weapon->getRules()->getPowerBonus(BattleActionAttack::GetAferShoot(_action, _ammo)) - _action.weapon->getRules()->getPowerRangeReduction(_range)
				: _ammo->getRules()->getPowerBonus(BattleActionAttack::GetAferShoot(_action, _ammo)) - _action.weapon->getRules()->getPowerRangeReduction(_range);
		}
		if (basePower < 0) basePower = 0;

		// Single random roll for this shot — same RNG that vanilla uses on every other hit.
		const int rolled = _ammo->getRules()->getDamageType()->getRandomDamage(basePower);
		_parent->getSave()->getBattleGame()->piercePower = rolled;

		// pWWWa/test: reset "last obstacle" markers for the new shot so the very first
		// terrain voxel the bullet enters is treated as a brand-new obstacle.
		_parent->getSave()->getBattleGame()->piercePrevTile = Position(-1, -1, -1);
		_parent->getSave()->getBattleGame()->piercePrevPart = -1;
		// pWWWa/test: reset backscan "last think pos" — first think() of this shot will
		// only test getPosition(0) (no history to scan yet).
		_parent->getSave()->getBattleGame()->piercePrevThinkPos = Position(-1, -1, -1);
		// pWWWa/test: reset SHATTER marker for the new shot.
		_parent->getSave()->getBattleGame()->pierceShatterAt   = Position(-1, -1, -1);
		_parent->getSave()->getBattleGame()->pierceShatterPart = -1;

		Log(LOG_INFO) << "[PIERCE] SHOT basePower=" << basePower
			<< " rolledDamage=" << rolled
			<< " origin=("    << _action.actor->getPosition().x << ',' << _action.actor->getPosition().y << ',' << _action.actor->getPosition().z << ')'
			<< " targetTile=("<< _action.target.x << ',' << _action.target.y << ',' << _action.target.z << ')'
			<< " targetVoxel=("<< _targetVoxel.x << ',' << _targetVoxel.y << ',' << _targetVoxel.z << ')'
			<< " (RNG of ammo's damageType)";
	}

	// Special handling for "spray" auto attack, get target positions from the action's waypoints, starting from the back
	if (_action.sprayTargeting)
	{
		// Since we're just spraying, target the middle of the tile
		_targetVoxel = _action.waypoints.back();
		Position targetPosition = _targetVoxel.toTile();

		// The waypoint targeting is possibly out of range of the gun, so move the voxel to the max range of the gun if it is
		int distanceSq = _action.actor->distance3dToPositionSq(targetPosition);
		if (_action.weapon->getRules()->isOutOfRange(distanceSq))
		{
			Position actorPosition = _action.actor->getPosition();
			int maxRange = _action.weapon->getRules()->getMaxRange();
			int distance = (int)std::ceil(sqrt(float(distanceSq)));
			_targetVoxel = (actorPosition + (targetPosition - actorPosition) * maxRange / distance).toVoxel() + TileEngine::voxelTileCenter;
			targetPosition = _targetVoxel.toTile();
		}

		// Turn at the end (to a potentially modified target position)
		_unit->lookAt(targetPosition, _unit->getTurretType() != -1);
		while (_unit->getStatus() == STATUS_TURNING)
		{
			_unit->turn(_unit->getTurretType() != -1);
		}

		_action.waypoints.pop_back();
	}

	// create a new projectile
	Projectile *projectile = new Projectile(_parent->getMod(), _parent->getSave(), _action, _origin, _targetVoxel, _ammo);

	// add the projectile on the map
	_parent->getMap()->setProjectile(projectile);

	// set the speed of the state think cycle to 16 ms (roughly one think cycle per frame)
	_parent->setStateInterval(1000/60);

	// let it calculate a trajectory
	_projectileImpact = V_EMPTY;

	double accuracyDivider = 100.0;
	// berserking units are half as accurate
	if (!_parent->getPanicHandled())
	{
		accuracyDivider = 200.0;
	}

	auto attack = BattleActionAttack::GetAferShoot(_action, _ammo);
	if (_action.type == BA_THROW)
	{
		_projectileImpact = projectile->calculateThrow(BattleUnit::getFiringAccuracy(attack, _parent->getMod()) / accuracyDivider);
		const RuleItem *ruleItem = _action.weapon->getRules();
		if (_projectileImpact == V_FLOOR || _projectileImpact == V_UNIT || _projectileImpact == V_OBJECT || _projectileImpact == V_WESTWALL || _projectileImpact == V_NORTHWALL || _projectileImpact == V_EMPTY)
		{
			if (_unit->getFaction() != FACTION_PLAYER && ruleItem->isGrenadeOrProxy())
			{
				_action.weapon->setFuseTimer(ruleItem->getFuseTimerDefault());
			}
			_action.weapon->moveToOwner(nullptr);
			if (_action.weapon->getGlow())
			{
				_parent->getTileEngine()->calculateLighting(LL_UNITS, _unit->getPosition());
				_parent->getTileEngine()->calculateFOV(_unit->getPosition(), _action.weapon->getGlowRange(), false);
			}
			_parent->getMod()->getSoundByDepth(_parent->getDepth(), Mod::ITEM_THROW)->play(-1, _parent->getMap()->getSoundAngle(_unit->getPosition()));
			if (!Mod::EXTENDED_EXPERIENCE_AWARD_SYSTEM)
			{
				// vanilla compatibility (throwing anything anywhere gives throwing exp)
				_unit->addThrowingExp();
			}
		}
		else
		{
			// unable to throw here
			delete projectile;
			_parent->getMap()->setProjectile(0);
			_action.result = "STR_UNABLE_TO_THROW_HERE";
			_action.clearTU();
			_parent->popState();
			return false;
		}
	}
	else if (_action.weapon && _action.weapon->getArcingShot(_action.type)) // special code for the "spit" trajectory
	{
		_projectileImpact = projectile->calculateThrow(BattleUnit::getFiringAccuracy(attack, _parent->getMod()) / accuracyDivider);
		if (_projectileImpact != V_EMPTY && _projectileImpact != V_OUTOFBOUNDS)
		{
			// set the soldier in an aiming position
			_unit->aim(true);
			// and we have a lift-off
			if (_ammo && _ammo->getRules()->getFireSound() != Mod::NO_SOUND)
			{
				_parent->getMod()->getSoundByDepth(_parent->getDepth(), _ammo->getRules()->getFireSound())->play(-1, _parent->getMap()->getSoundAngle(_unit->getPosition()));
			}
			else if (_action.weapon->getRules()->getFireSound() != Mod::NO_SOUND)
			{
				_parent->getMod()->getSoundByDepth(_parent->getDepth(), _action.weapon->getRules()->getFireSound())->play(-1, _parent->getMap()->getSoundAngle(_unit->getPosition()));
			}
			if (_action.type != BA_LAUNCH)
			{
				_action.weapon->spendAmmoForAction(_action.type, _parent->getSave());
			}
		}
		else
		{
			// no line of fire
			delete projectile;
			_parent->getMap()->setProjectile(0);
			if (_parent->getPanicHandled())
			{
				_action.result = "STR_NO_TRAJECTORY";
			}
			_unit->abortTurn();
			_parent->popState();
			return false;
		}
	}
	else
	{

		if (_originVoxel != TileEngine::invalid)
		{
			_projectileImpact = projectile->calculateTrajectory(BattleUnit::getFiringAccuracy(attack, _parent->getMod()) / accuracyDivider, _originVoxel, false);
		}
		else
		{
			_projectileImpact = projectile->calculateTrajectory(BattleUnit::getFiringAccuracy(attack, _parent->getMod()) / accuracyDivider);
		}
		if (_targetVoxel != TileEngine::invalid.toVoxel() && (_projectileImpact != V_EMPTY || _action.type == BA_LAUNCH))
		{
			// set the soldier in an aiming position
			_unit->aim(true);
			// and we have a lift-off
			if (_ammo && _ammo->getRules()->getFireSound() != Mod::NO_SOUND)
			{
				_parent->getMod()->getSoundByDepth(_parent->getDepth(), _ammo->getRules()->getFireSound())->play(-1, _parent->getMap()->getSoundAngle(projectile->getOrigin()));
			}
			else if (_action.weapon && _action.weapon->getRules()->getFireSound() != Mod::NO_SOUND)
			{
				_parent->getMod()->getSoundByDepth(_parent->getDepth(), _action.weapon->getRules()->getFireSound())->play(-1, _parent->getMap()->getSoundAngle(projectile->getOrigin()));
			}
			if (_action.type != BA_LAUNCH)
			{
				_action.weapon->spendAmmoForAction(_action.type, _parent->getSave());
			}
		}
		else
		{
			// no line of fire
			delete projectile;
			_parent->getMap()->setProjectile(0);
			if (_parent->getPanicHandled())
			{
				_action.result = "STR_NO_LINE_OF_FIRE";
			}
			_unit->abortTurn();
			_parent->popState();
			return false;
		}
	}

	if (_action.type != BA_THROW && _action.type != BA_LAUNCH)
		_unit->getStatistics()->shotsFiredCounter++;

	// hit log - new bullet
	if (_action.actor)
	{
		_parent->getSave()->appendToHitLog(HITLOG_NEW_SHOT, _action.actor->getFaction());
	}

	return true;
}

/**
 * Deinitialize the state.
 */
void ProjectileFlyBState::deinit()
{
	_parent->getMap()->setFollowProjectile(true); // turn back on when done shooting
}

/**
 * Animates the projectile (moves to the next point in its trajectory).
 * If the animation is finished the projectile sprite is removed from the map,
 * and this state is finished.
 */
void ProjectileFlyBState::think()
{
	/// checks if a weapon has any more shots to fire.
	auto noMoreShotsToShoot = [this]()
		{
		return _action.type != BA_AKIMBOSHOT
			   ? (!_action.weapon->haveNextShotsForAction(_action.type, _action.autoShotCounter) || !_action.weapon->getAmmoForAction(_action.type))
			   : (_action.actWeaponCounter >= _action.actWeaponShotQnty && _action.opWeaponCounter >= _action.opWeaponShotQnty);
		};

	_parent->getSave()->getBattleState()->clearMouseScrollingState();
	/* TODO refactoring : store the projectile in this state, instead of getting it from the map each time? */
	if (_parent->getMap()->getProjectile() == 0)
	{
		bool hasFloor = _action.actor->haveNoFloorBelow() == false;
		bool unitCanFly = _action.actor->getMovementType() == MT_FLY;

		if ( ( _action.type != BA_AKIMBOSHOT
			? (_action.weapon->haveNextShotsForAction(_action.type, _action.autoShotCounter) && _ammo->getAmmoQuantity() != 0)
			: (_action.actWeaponCounter < _action.actWeaponShotQnty || _action.opWeaponCounter < _action.opWeaponShotQnty ) )
			&& !_action.actor->isOut()
			&& (hasFloor || unitCanFly) )
		{
			createNewProjectile();
			if (_action.cameraPosition.z != -1)
			{
				_parent->getMap()->getCamera()->setMapOffset(_action.cameraPosition);
				_parent->getMap()->invalidate();
			}
		}
		else
		{
			if (_action.cameraPosition.z != -1 && _action.waypoints.size() <= 1)
			{
				_parent->getMap()->getCamera()->setMapOffset(_action.cameraPosition);
				_parent->getMap()->invalidate();
			}
			if (!_parent->getSave()->getUnitsFalling() && _parent->getPanicHandled())
			{
				_parent->getTileEngine()->checkReactionFire(_unit, _action);
			}
			if (!_unit->isOut())
			{
				_unit->abortTurn();
			}
			if (_parent->getSave()->getSide() == FACTION_PLAYER || _parent->getSave()->getDebugMode())
			{
				_parent->setupCursor();
			}
			_parent->convertInfected();
			_parent->popState();
		}
	}
	else
	{
		auto attack = BattleActionAttack::GetAferShoot(_action, _ammo);

		// pWWWa/test: pierce bullet handling.
		//
		// STEP 1 (this version): outcome #1 only for terrain — projectile passes through the
		// obstacle, the obstacle STAYS INTACT, the bullet just loses pierce capacity (and the
		// usual range-based power dropoff still applies). When piercePower drops to <=0 the
		// loop in Projectile::move() halts the projectile and the normal "impact!" path below
		// will spawn the final ExplosionBState on the tile where it actually stopped.
		// Units (V_UNIT) keep the previous behaviour: a real randomized TileEngine::hit()
		// (side armor, resistances, scripts, etc.).
		//
		// Important: think() runs every frame while the projectile is alive, so a bullet that
		// sits inside the same wall voxel for several ticks (e.g. while move() is processing
		// the rest of its speed budget) MUST NOT pay pierce capacity more than once for that
		// obstacle. We track the last (tile, part) pair we charged the bullet for in
		// BattlescapeGame::piercePrevTile / piercePrevPart, and only charge again when the
		// bullet moves on to a NEW obstacle.
		if (_ammo && _ammo->getRules()->getPierceType()
			&& !_ammo->getRules()->getShotgunPellets()
			&& _parent->getMap()->getProjectile())
		{
			Projectile* proj  = _parent->getMap()->getProjectile();
			auto*       bgame = _parent->getSave()->getBattleGame();
			const auto  dmgAOE = _ammo->getRules()->getPierceAOEDamageType();

			// pWWWa/test: backscan. Projectile::move() advances by _speed voxels per
			// think() tick. A high-speed bullet (sniper rifles + custom bulletSpeed)
			// can skip narrow single-voxel obstacles like westwalls / northwalls
			// entirely between two think() calls — voxelCheck() at getPosition(0)
			// then returns V_EMPTY for the empty voxel BEHIND the wall, and the
			// obstacle is silently ignored. Confirmed in logs of 2026-06-02:
			// projectile flew through (44,18) and (31,18) walls without producing
			// any [PIERCE] NEW entries (just SHOT -> END V_OUTOFBOUNDS).
			//
			// Fix: scan ALL voxels the projectile crossed since the previous tick,
			// from oldest to newest, and process each one through the same single-
			// voxel handler. piercePrevTile/piercePrevPart dedup ensures the same
			// wall is charged once even if backscan finds it in two adjacent voxels.
			//
			// We use getPosition(-N) helpers; the trajectory buffer caps at the
			// real length so out-of-range indices clamp safely. kBackscanDepth=12
			// covers any realistic _speed value.
			static constexpr int kBackscanDepth = 12;

			// Handler for ONE voxel position. Returns nothing; sets _projectileImpact
			// to the voxel type that was processed (or last-seen empty).
			auto handlePierceVoxel = [&](const Position& pos)
			{
				_projectileImpact = _parent->getTileEngine()->voxelCheck(pos, _unit);

				Tile*      tile = _parent->getSave()->getTile(pos.toTile());
				const auto tp   = static_cast<TilePart>(_projectileImpact);

				// pWWWa/test: floor (V_FLOOR) counts as a real obstacle too. Diagonal/vertical
				// shots cross floors and roofs as legitimate pierce targets (think of firing
				// down through a hole in the roof, or up through a second-storey floor).
				// So the full V_FLOOR..V_UNIT range is in.
				if (!(_projectileImpact >= V_FLOOR && _projectileImpact <= V_UNIT))
				{
					return; // empty/out-of-bounds voxel, nothing to do
				}
				if (bgame->piercePower <= 0)
				{
					return; // bullet already spent — let Projectile::move() stop it
				}
				if (!tile)
				{
					return;
				}

				const Position obstacleTile = pos.toTile();
				const int      obstaclePart = (int)_projectileImpact;
				const bool     sameAsPrev   = (obstacleTile == bgame->piercePrevTile
											  && obstaclePart == bgame->piercePrevPart);

				if (sameAsPrev)
				{
					// We already charged this exact obstacle. No power loss, no log spam.
					return;
				}
				{
					// ===== Excel-formula pierce model =====================================
					// piercePower for a pierce bullet IS the current damage of the bullet
					// (rolled once at the muzzle in createNewProjectile()).
					//
					//   currentDamage = bgame->piercePower
					//   AE            = ammo.damageType.ArmorEffectiveness
					//   ToTile        = ammo.damageType.ToTile
					//   armor         = tile.getMapData(part).getArmor()
					//
					// For terrain (V_FLOOR / V_WALL / V_OBJECT):
					//   baseDecr      = armor * AE                 (Excel "Базовое торможение")
					//   finalDecr     = sqrt(baseDecr^3 / damage)  (Excel "Итоговое торможение")
					//   damageToWall  = damage * AE^2 * ToTile     (Excel "Урон забору" * ToTile)
					// New damage carried forward     = damage - finalDecr.
					// Wear accumulates per (tile, part); destroyed when wear >= armor * MUL.
					//
					// For V_UNIT we keep the original "(armor*AE + health) / resist" decrement
					// so a bullet eventually runs out of damage on tough enemies, but the
					// actual hit goes through TileEngine::hitUnit() with the current damage
					// directly (no second random roll inside the hit). =========================

					const auto* dtype  = _ammo->getRules()->getDamageType();
					const float AE     = dtype->ArmorEffectiveness;
					const float ToTile = dtype->ToTile;
					const int   damage = bgame->piercePower; // current damage carried by bullet

					int  piercePowerDecrement = 0;
					int  tileArmor            = 0;
					int  damageToWall         = 0;
					int  appliedToUnit        = 0;

					if (_projectileImpact == V_UNIT
						&& tile->getOverlappingUnit(_parent->getSave())
						&& tile->getOverlappingUnit(_parent->getSave())->getHealth() > 0)
					{
						// UNIT: unchanged decrement (front armor + remaining health).
						BattleUnit* tgt    = tile->getOverlappingUnit(_parent->getSave());
						const float resist = tgt->getArmor()->getDamageModifier(dtype->ResistType);
						piercePowerDecrement =
							(int)((tgt->getArmor()->getArmor(SIDE_FRONT) * AE + tgt->getHealth())
							/ (resist > 0.0f ? resist : 1.0f));
						appliedToUnit = damage; // we'll hit unit with whatever the bullet has now
					}
					else
					{
						tileArmor = tile->getMapData(tp) ? tile->getMapData(tp)->getArmor() : 0;
						// ===== Excel-formula v3 (two-threshold model) =======================
						// AE classification (informative, not used in math):
						//   AE < 1.0  — armor-piercing rounds (steel/tungsten core)
						//   AE = 1.0  — standard FMJ (copper jacket)
						//   AE > 1.0  — expanding / soft-lead rounds
						//
						// Two thresholds:
						//   T1 = (HP^2 / 80) * AE                      [scratch threshold]
						//   T2 = MAX(T1 * 1.2 ; HP * AE)               [full-pierce threshold]
						//
						// Three behaviour zones, by current bullet damage D:
						//   D <= T1                 -> SHATTER. Wall takes 0. Bullet stops here.
						//   T1 < D < T2             -> SCRATCH. Linear interpolation between
						//                              "nothing" at T1 and "full" at T2.
						//                              fullWall = T2 * AE − HP * 0.1
						//                              coef     = (D − T1) / (T2 − T1)
						//                              rawWall  = fullWall * coef
						//                              Bullet still suffers the full T2-based
						//                              drag below, so a bullet just over T1
						//                              will get stopped inside the wall on its
						//                              own — no extra logic needed.
						//   D >= T2                 -> FULL PIERCE.
						//                              rawWall  = D * AE − HP * 0.1
						//
						// damageToWall = round(rawWall * ToTile).
						//
						// Bullet drag (when threshold T1 passed):
						//   finalDecr = T2 * (T2 / D) ^ 0.4
						// T2 is used here on purpose for a seamless numeric transition between
						// scratch and full-pierce zones (no jump in drag at D = T2). When D is
						// only just above T1, (T2/D) is well above 1 and finalDecr ends up
						// greater than D, so the bullet naturally gets stuck in the wall —
						// matches the Excel "вмятинка, пуля внутри стены" picture.
						// ====================================================================
						if (AE > 0.0f && tileArmor > 0 && damage > 0)
						{
							const double T1 = ((double)tileArmor * tileArmor / 80.0) * AE;
							const double T2 = std::max(T1 * 1.2, (double)tileArmor * AE);

							if ((double)damage <= T1)
							{
								// Zone A: SHATTER. Bullet disintegrates on the surface.
								damageToWall         = 0;
								piercePowerDecrement = damage; // force piercePower to 0

								// pWWWa/test: remember the EXACT voxel on the obstacle's
								// surface so the impact-ExplosionBState below spawns at the
								// right spot regardless of where Projectile::move() finally
								// halts the bullet. Also flush remaining trajectory so the
								// projectile visually freezes here without overshoot.
								bgame->pierceShatterAt   = pos;
								bgame->pierceShatterPart = obstaclePart;
								if (proj) proj->skipTrajectory();
							}
							else
							{
								double rawWall;
								if ((double)damage < T2)
								{
									// Zone B: SCRATCH (T1 < D < T2). Linear interpolation.
									const double fullWall = T2 * AE - (double)tileArmor * 0.1;
									const double coef     = ((double)damage - T1) / (T2 - T1);
									rawWall               = fullWall * coef;
								}
								else
								{
									// Zone C: FULL PIERCE (D >= T2).
									rawWall = (double)damage * AE - (double)tileArmor * 0.1;
								}
								if (rawWall < 0.0) rawWall = 0.0;
								damageToWall = (int)std::round(rawWall * ToTile);

								// Bullet drag — same formula for Zones B and C (T2-based).
								const double ratio    = T2 / (double)damage;
								const double finalDec = T2 * std::pow(ratio, 0.4);
								piercePowerDecrement  = (int)std::round(finalDec);
							}
						}
						// AE == 0 -> "нейтринная" пуля: 0 decrement, 0 wear, летит сквозь.
					}

					// ----- terrain wear / destruction (OUTCOME 2) -----
					bool destroyedNow  = false;
					int  wearBefore    = 0;
					int  wearAfter     = 0;
					int  wearThreshold = 0;
					if (_projectileImpact != V_UNIT && tileArmor > 0 && damageToWall > 0)
					{
						const int tileKey = (obstacleTile.z * 256 + obstacleTile.y) * 256 + obstacleTile.x;
						const std::pair<int, int> key(tileKey, obstaclePart);

						auto it       = bgame->pierceWear.find(key);
						wearBefore    = (it != bgame->pierceWear.end()) ? it->second : 0;
						wearAfter     = wearBefore + damageToWall;
						wearThreshold = tileArmor * BattlescapeGame::PIERCE_DESTROY_MULTIPLIER;

						if (wearAfter >= wearThreshold)
						{
							destroyedNow = true;
							bgame->pierceWear.erase(key);

							// Replicate the side effects of TileEngine::hit() for terrain:
							// 1) base-defense module bookkeeping
							if (tp == O_OBJECT
								&& _parent->getSave()->getMissionType() == "STR_BASE_DEFENSE"
								&& tile->getMapData(O_OBJECT)
								&& tile->getMapData(O_OBJECT)->isBaseModule())
							{
								auto& mm = _parent->getSave()->getModuleMap();
								mm[(pos.x / 16) / 10][(pos.y / 16) / 10].second--;
							}
							// 2) Tile::damage(part, value, obj) with value >= armor -> destroyed
							if (tile->damage(tp, tileArmor, _parent->getSave()->getObjectiveType()))
							{
								_parent->getSave()->addDestroyedObjective();
							}
							// 3) refresh visibility / lighting / gravity
							_parent->getTileEngine()->applyGravity(tile);
							LightLayers layer = (tp == O_FLOOR
								&& _parent->getSave()->getTile(obstacleTile - Position(0, 0, 1)))
								? LL_AMBIENT : LL_FIRE;
							_parent->getTileEngine()->calculateLighting(layer, obstacleTile, 1, true);
							_parent->getTileEngine()->calculateFOV(obstacleTile, 1, true, true);
						}
						else
						{
							bgame->pierceWear[key] = wearAfter;
						}
					}

					if (_projectileImpact == V_UNIT)
					{
						// Hit the unit directly with the bullet's CURRENT damage.
						// No double random: getRandomDamage was already rolled at the muzzle.
						BattleUnit* tgt = tile->getOverlappingUnit(_parent->getSave());
						const int sz    = tgt ? tgt->getArmor()->getSize() * 8 : 8;
						const Position relative = pos - (tgt
							? tgt->getPosition().toVoxel() + Position(sz, sz,
								tgt->getFloatHeight() - tile->getTerrainLevel())
							: pos);
						_parent->getSave()->getTileEngine()->hitUnit(
							attack, tgt, relative, appliedToUnit,
							dtype->isDirect()
								? dtype
								: _parent->getMod()->getDamageType(dmgAOE));
					}
					// Terrain: handled above (either destroyed or just wear accumulated).

					// ----- spend bullet damage AFTER applying effects -----
					bgame->piercePower -= piercePowerDecrement;

					// ----- remember this obstacle so we don't charge it again next tick -----
					bgame->piercePrevTile = obstacleTile;
					bgame->piercePrevPart = obstaclePart;

					// ----- diagnostics (grep the log for "PIERCE") -----
					// Recompute T1/T2 for diagnostics (cheap, deterministic — same numbers
					// the formula block above used).
					const double t1Log = (tileArmor > 0 && AE > 0.0f)
						? ((double)tileArmor * tileArmor / 80.0) * AE
						: 0.0;
					const double t2Log = (tileArmor > 0 && AE > 0.0f)
						? std::max(t1Log * 1.2, (double)tileArmor * AE)
						: 0.0;

					const char* outcomeTag;
					if (_projectileImpact == V_UNIT)
					{
						outcomeTag = " => UNIT HIT";
					}
					else if (destroyedNow)
					{
						outcomeTag = " => TERRAIN OUTCOME 2 (PASS-THROUGH + DESTROYED)";
					}
					else if (bgame->piercePower > 0)
					{
						outcomeTag = " => TERRAIN OUTCOME 1 (PASS-THROUGH, obstacle SURVIVES)";
					}
					else if (tileArmor > 0 && AE > 0.0f && (double)damage <= t1Log)
					{
						// Zone A: D <= T1, bullet shattered on the surface.
						outcomeTag = " => TERRAIN OUTCOME 4 (SHATTERED, obstacle UNHARMED)";
					}
					else
					{
						// Either Zone B (scratch + stuck) or rare Zone C corner case.
						outcomeTag = " => TERRAIN OUTCOME 3 (STOPPED on obstacle)";
					}

					// pWWWa/test: in SHATTER (Zone A) wearAfter / wearThreshold are both
					// zero by design — we never even compute them — so the bare "(0/0)" in
					// the log used to look like a missing value. Print "(SHATTER)" instead
					// to make it obvious that the bullet shattered on the surface.
					const bool shatterLog = (tileArmor > 0 && AE > 0.0f && (double)damage <= t1Log);
					std::ostringstream wearTag;
					if (shatterLog)
						wearTag << "SHATTER";
					else
						wearTag << wearAfter << '/' << wearThreshold;

					Log(LOG_INFO) << "[PIERCE] NEW tile=(" << obstacleTile.x << ',' << obstacleTile.y << ',' << obstacleTile.z << ')'
						<< " vox=("       << pos.x << ',' << pos.y << ',' << pos.z << ')'
						<< " part="       << obstaclePart
						<< " AE="         << AE
						<< " ToTile="     << ToTile
						<< " armor="      << tileArmor
						<< " T1="         << t1Log
						<< " T2="         << t2Log
						<< " damageIn="   << damage
						<< " finalDecr="  << piercePowerDecrement
						<< " wear+="      << damageToWall << " (" << wearTag.str() << ')'
						<< " damageOut="  << bgame->piercePower
						<< outcomeTag;

					if (_projectileImpact == V_UNIT)
					{
						if (!_parent->areAllEnemiesNeutralized()) projectileHitUnit(pos);
						_parent->checkForCasualties(nullptr, attack);
						_parent->getSave()->reviveUnconsciousUnits(true);
						_parent->convertInfected();
						_parent->setStateInterval(BattlescapeState::DEFAULT_ANIM_SPEED / 5);
					}
					else if (destroyedNow)
					{
						// If the destroyed part was HE/explosive itself it might cascade.
						if (tile->getSavedGame()->getTileEngine()->checkForTerrainExplosions())
						{
							_parent->statePushNext(new ExplosionBState(_parent, proj->getLastPositions(),
								BattleActionAttack{ BA_NONE, attack.attacker }, tile, false, 0, 0));
						}
					}
				}
			}; // end of handlePierceVoxel lambda

			// pWWWa/test: backscan over voxels crossed since last think() tick.
			// Walk from oldest (-N) to newest (0). N = min(kBackscanDepth, distance
			// from current voxel to previous-tick voxel). If we never ran think()
			// before for this shot, N = kBackscanDepth (just be safe).
			const Position currentVox = proj->getPosition(0);
			int scanFrom = kBackscanDepth;
			if (bgame->piercePrevThinkPos.x >= 0)
			{
				// Find at what negative offset getPosition() matches piercePrevThinkPos.
				// We don't have direct access to the trajectory vector here, so we just
				// cap the scan at kBackscanDepth — duplicates are filtered by
				// piercePrevTile/piercePrevPart dedup inside handlePierceVoxel.
				const int dx = std::abs(currentVox.x - bgame->piercePrevThinkPos.x);
				const int dy = std::abs(currentVox.y - bgame->piercePrevThinkPos.y);
				const int dz = std::abs(currentVox.z - bgame->piercePrevThinkPos.z);
				const int travelled = std::max({dx, dy, dz}); // Chebyshev distance in voxels
				scanFrom = std::min(kBackscanDepth, std::max(0, travelled));
			}
			for (int back = scanFrom; back >= 0; --back)
			{
				if (bgame->piercePower <= 0) break; // stop scanning if bullet is spent
				const Position scanPos = proj->getPosition(-back);
				handlePierceVoxel(scanPos);
			}
			// Remember where we ended this think() for the next backscan.
			bgame->piercePrevThinkPos = currentVox;

			// If after backscan the current voxel is empty/out-of-bounds and there is
			// no living projectile state, set _projectileImpact accordingly so the
			// "impact!" path below (when projectile->move() returns false) behaves
			// like before — V_EMPTY for LAUNCH chain, V_OUTOFBOUNDS for normal end.
			if (_projectileImpact < V_FLOOR || _projectileImpact > V_UNIT)
			{
				_projectileImpact = _action.type == BA_LAUNCH && _action.waypoints.size() > 1
					? V_EMPTY : V_OUTOFBOUNDS;
			}
		}

		if (_action.type != BA_THROW && _ammo && _ammo->getRules()->getShotgunPellets() != 0)
		{
			// shotgun pellets move to their terminal location instantly as fast as possible
			_parent->getMap()->getProjectile()->skipTrajectory();
		}
		if (!_parent->getMap()->getProjectile()->move())
		{
			if (_ammo && _ammo->getRules()->isOutOfRange(_action.actor->distance3dToPositionSq(_parent->getMap()->getProjectile()->getPosition().toTile())))
			{ // Projectile has special maxRange event property, when stopped at restricted range, let handle it
				switch (_ammo->getRules()->getMaxRangeEvent())
				{
					case 1:	_projectileImpact = V_EMPTY; break; // generate explosion
					case 2:	_projectileImpact = V_OUTOFBOUNDS;  // vanish
				}
			}
			// impact !
			if (_action.type == BA_THROW)
			{
				_parent->getMap()->resetCameraSmoothing();
				Position pos = _parent->getMap()->getProjectile()->getPosition(Projectile::ItemDropVoxelOffset).toTile();
				if (pos.y > _parent->getSave()->getMapSizeY())
				{
					pos.y--;
				}
				if (pos.x > _parent->getSave()->getMapSizeX())
				{
					pos.x--;
				}

				_parent->getMod()->getSoundByDepth(_parent->getDepth(), Mod::ITEM_DROP)->play(-1, _parent->getMap()->getSoundAngle(pos));
				const RuleItem *ruleItem = _action.weapon->getRules();
				if (_action.weapon->fuseThrowEvent())
				{
					if (ruleItem->getBattleType() == BT_GRENADE || ruleItem->getBattleType() == BT_PROXIMITYGRENADE)
					{
						// it's a hot grenade to explode immediately
						_parent->statePushFront(new ExplosionBState(_parent, _parent->getMap()->getProjectile()->getLastPositions(Projectile::ItemDropVoxelOffset), attack));
					}
					else
					{
						_parent->getSave()->removeItem(_action.weapon);
					}
				}
				else
				{
					_parent->dropItem(pos, _action.weapon);
					if (_unit->isAIControlled() && ruleItem->isGrenadeOrProxy())
					{
						_parent->getTileEngine()->setDangerZone(pos, ruleItem->getExplosionRadius(attack), _action.actor);
					}
				}
			}
			else if (_action.type == BA_LAUNCH && _action.waypoints.size() > 1 && _projectileImpact == V_EMPTY)
			{
				_origin = _action.waypoints.front();
				_action.waypoints.pop_front();
				_action.target = _action.waypoints.front();
				// launch the next projectile in the waypoint cascade
				ProjectileFlyBState *nextWaypoint = new ProjectileFlyBState(_parent, _action, _origin, _range + _parent->getMap()->getProjectile()->getDistance());
				nextWaypoint->setOriginVoxel(_parent->getMap()->getProjectile()->getPosition(-1));
				if (_origin == _action.target)
				{
					nextWaypoint->targetFloor();
				}
				_parent->statePushNext(nextWaypoint);
			}
			else
			{
				auto tmpUnit = _parent->getSave()->getTile(_action.target)->getUnit();
				if (tmpUnit && tmpUnit != _unit)
				{
					tmpUnit->getStatistics()->shotAtCounter++; // Only counts for guns, not throws or launches
				}

				_parent->getMap()->resetCameraSmoothing();
				if (_action.type == BA_LAUNCH)
				{
					_action.weapon->spendAmmoForAction(_action.type, _parent->getSave());
				}

				if (_projectileImpact != V_OUTOFBOUNDS || (_ammo && _ammo->getRules()->getShotgunPellets()))
				{
					bool shotgun = _ammo && _ammo->getRules()->getShotgunPellets() != 0 && _ammo->getRules()->getDamageType()->isDirect();
					int offset = 0;
					// explosions impact not inside the voxel but two steps back (projectiles generally move 2 voxels at a time)
					if (_ammo && _ammo->getRules()->getExplosionRadius(attack) != 0 && _projectileImpact != V_UNIT)
					{
						offset = -2;
					}

					// pWWWa/test: if this pierce bullet shattered (Zone A), the hit
					// animation MUST appear on the obstacle's surface, not at wherever
					// Projectile::move() happened to stop. Override the impact center
					// with the SHATTER voxel we recorded earlier.
					auto* _bgame = _parent->getSave()->getBattleGame();
					LastPositions impactCenter = _parent->getMap()->getProjectile()->getLastPositions(offset);
					if (_bgame->pierceShatterAt.x >= 0)
					{
						impactCenter = LastPositions(_bgame->pierceShatterAt, _bgame->pierceShatterAt);
						// Also lock _projectileImpact to the actual obstacle part so the
						// branch below doesn't accidentally fall into V_OUTOFBOUNDS code.
						_projectileImpact = (VoxelType)_bgame->pierceShatterPart;
						Log(LOG_INFO) << "[PIERCE] SHATTER impact override at vox=("
							<< _bgame->pierceShatterAt.x << ','
							<< _bgame->pierceShatterAt.y << ','
							<< _bgame->pierceShatterAt.z << ") part="
							<< _bgame->pierceShatterPart;
					}

					_parent->statePushFront(new ExplosionBState(
						_parent, impactCenter,
						attack, 0,
						noMoreShotsToShoot(),
						shotgun ? 0 : _range + _parent->getMap()->getProjectile()->getDistance()
					));

					if (_projectileImpact == V_OUTOFBOUNDS)
					{ // Remove hit animation for first pellet during "shotgun void hit event"
						_parent->getMap()->getExplosions()->clear();
					}

					if (_projectileImpact == V_UNIT)
					{
						projectileHitUnit(_parent->getMap()->getProjectile()->getPosition(offset));
					}

					// remember unit's original XP values, used for nerfing below
					_unit->rememberXP();

					// special shotgun behaviour: trace extra projectile paths, and add bullet hits at their termination points.
					if (shotgun)
					{
						int behaviorType = _ammo->getRules()->getShotgunBehaviorType();
						int spread = _ammo->getRules()->getShotgunSpread();
						int choke = _action.weapon->getRules()->getShotgunChoke();
						Position firstPelletImpact = _parent->getMap()->getProjectile()->getPosition(-2);
						Position originalTarget = _targetVoxel;

						int i = 1;
						while (i != _ammo->getRules()->getShotgunPellets())
						{
							if (behaviorType == 1)
							{
								// use impact location to determine spread (instead of originally targeted voxel), as long as it's not the same as the origin
								if (firstPelletImpact != _parent->getSave()->getTileEngine()->getOriginVoxel(_action, _parent->getSave()->getTile(_origin)))
								{
									_targetVoxel = firstPelletImpact;
								}
								else
								{
									_targetVoxel = originalTarget;
								}
							}


							Projectile *proj = new Projectile(_parent->getMod(), _parent->getSave(), _action, _origin, _targetVoxel, _ammo);

							// let it trace to the point where it hits
							int secondaryImpact = V_EMPTY;
							if (behaviorType == 1)
							{
								// pellet spread based on spread and choke values
								secondaryImpact = proj->calculateTrajectory(std::max(0.0, (1.0 - spread / 100.0) * choke / 100.0));

							}
							else
							{
								// pellet spread based on spread and firing accuracy with diminishing formula
								// identical with vanilla formula when spread = 100 (default)
								secondaryImpact = proj->calculateTrajectory(std::max(0.0, (BattleUnit::getFiringAccuracy(attack, _parent->getMod()) / 100.0) - i * 5.0 * spread / 100.0));
							}

							if (secondaryImpact != V_EMPTY)
							{
								// as above: skip the shot to the end of it's path
								proj->skipTrajectory();
								// insert an explosion and hit
								if (secondaryImpact != V_OUTOFBOUNDS)
								{
									if (secondaryImpact == V_UNIT)
									{
										projectileHitUnit(proj->getPosition(offset));
									}
									//Explosion *explosion = new Explosion(proj->getPosition(offset), _ammo->getRules()->getHitAnimation(), 0, false, false, _ammo->getRules()->getHitAnimationFrames());
									int power = 0;
									if (_action.weapon->getRules()->getIgnoreAmmoPower())
									{
										power = _action.weapon->getRules()->getPowerBonus(attack) - _action.weapon->getRules()->getPowerRangeReduction(proj->getDistance());
									}
									else
									{
										power = _ammo->getRules()->getPowerBonus(attack) - _ammo->getRules()->getPowerRangeReduction(proj->getDistance());
									}
									_parent->getMap()->getExplosions()->push_back(new Explosion(proj->getPosition(offset), _ammo->getRules()->getHitAnimation(), 0, false, false, _ammo->getRules()->getHitAnimationFrames()));
									_parent->getSave()->getTileEngine()->hit(attack, proj->getPosition(offset), power, _ammo->getRules()->getDamageType());

									//do not work yet
//									if (_ammo->getRules()->getExplosionRadius(_unit) != 0)
//									{
//										_parent->getTileEngine()->explode({ _action, _ammo }, proj->getPosition(offset), _ammo->getRules()->getPower(), _ammo->getRules()->getDamageType(), _ammo->getRules()->getExplosionRadius(), _unit);
//									}
								}
							}
							++i;
							delete proj;
						}

						// reset back for the next shot in the (potential) autoshot sequence
						_targetVoxel = originalTarget;
					}

					// nerf unit's XP values (gained via extra shotgun bullets)
					_unit->nerfXP();
				}
				else if (noMoreShotsToShoot())
				{
					_unit->aim(false);
				}
			}

			// pWWWa/test: pierce diagnostic — projectile end-of-life marker. Pairs with
			// the [PIERCE] SHOT line from createNewProjectile() so it's obvious whether a
			// shot really hit nothing (SHOT followed directly by END with no NEWs between)
			// versus the rest of the log just being elsewhere on screen.
			if (_ammo && _ammo->getRules()->getPierceType() && !_ammo->getRules()->getShotgunPellets())
			{
				const char* impactName =
					(_projectileImpact == V_EMPTY)       ? "V_EMPTY"
					: (_projectileImpact == V_FLOOR)     ? "V_FLOOR"
					: (_projectileImpact == V_WESTWALL)  ? "V_WESTWALL"
					: (_projectileImpact == V_NORTHWALL) ? "V_NORTHWALL"
					: (_projectileImpact == V_OBJECT)    ? "V_OBJECT"
					: (_projectileImpact == V_UNIT)      ? "V_UNIT"
					: (_projectileImpact == V_OUTOFBOUNDS) ? "V_OUTOFBOUNDS"
					: "?";
				Projectile* finalProj = _parent->getMap()->getProjectile();
				const Position finalVox = finalProj ? finalProj->getPosition() : Position(-1,-1,-1);
				Log(LOG_INFO) << "[PIERCE] END  piercePowerLeft=" << _parent->getSave()->getBattleGame()->piercePower
					<< " finalImpact=" << _projectileImpact << " (" << impactName << ')'
					<< " finalVox=("  << finalVox.x << ',' << finalVox.y << ',' << finalVox.z << ')'
					<< " finalTile=(" << finalVox.toTile().x << ',' << finalVox.toTile().y << ',' << finalVox.toTile().z << ')';
			}

			delete _parent->getMap()->getProjectile();
			_parent->getMap()->setProjectile(0);
		}
	}
}

/**
 * Flying projectiles cannot be cancelled,
 * but they can be "skipped".
 */
void ProjectileFlyBState::cancel()
{
	if (_parent->getMap()->getProjectile())
	{
		_parent->getMap()->getProjectile()->skipTrajectory();
		Position p = _parent->getMap()->getProjectile()->getPosition().toTile();
		if (!_parent->getMap()->getCamera()->isOnScreen(p, false, 0, false))
			_parent->getMap()->getCamera()->centerOnPosition(p);
	}
	if (_parent->areAllEnemiesNeutralized())
	{
		// stop autoshots and akimbo shots when battle auto-ends
		_action.autoShotCounter = 1000;
		_action.actWeaponCounter = 1000;
		_action.opWeaponCounter = 1000;

		// Rationale: if there are any fatally wounded soldiers
		// the game still allows the player to resume playing the current turn (and heal them)
		// but we don't want to resume auto-shooting (it just looks silly)
	}
}

/**
 * Validates the throwing range.
 * @param action Pointer to throw action.
 * @param origin Position to throw from.
 * @param target Tile to throw to.
 * @param depth Battlescape depth.
 * @return True when the range is valid.
 */
bool ProjectileFlyBState::validThrowRange(BattleAction *action, Position origin, Tile *target, int depth)
{
	// note that all coordinates and thus also distances below are in number of tiles (not in voxels).
	if (action->type != BA_THROW)
	{
		return true;
	}
	int xdiff = action->target.x - action->actor->getPosition().x;
	int ydiff = action->target.y - action->actor->getPosition().y;
	int realDistanceSq = (xdiff * xdiff) + (ydiff * ydiff);

	int compatibilityDistanceSq = action->actor->distance3dToPositionSq(action->target); // 3d distance for compatibility with Map::drawTerrain()
	if (action->weapon->getRules()->isOutOfThrowRange(compatibilityDistanceSq, depth))
	{
		// if out of item's throw range, stop... no need to check weight- and strength-based range
		return false;
	}

	double realDistance = sqrt((double)realDistanceSq);

	int offset = 2;
	int zd = (origin.z)-((action->target.z * 24 + offset) - target->getTerrainLevel());
	int weight = action->weapon->getTotalWeight();
	double maxDistance = (getMaxThrowDistance(weight, action->actor->getBaseStats()->strength, zd) + 8) / 16.0;

	if (depth > 0 && Mod::EXTENDED_UNDERWATER_THROW_FACTOR > 0)
	{
		maxDistance = maxDistance * (double)Mod::EXTENDED_UNDERWATER_THROW_FACTOR / 100.0;
	}

	return realDistance <= maxDistance;
}

/**
 * Validates the throwing range.
 * @param weight the weight of the object.
 * @param strength the strength of the thrower.
 * @param level the difference in height between the thrower and the target.
 * @return the maximum throwing range.
 */
int ProjectileFlyBState::getMaxThrowDistance(int weight, int strength, int level)
{
	double curZ = level + 0.5;
	double dz = 1.0;
	int dist = 0;
	while (dist < 4000) //just in case
	{
		dist += 8;
		if (dz<-1)
			curZ -= 8;
		else
			curZ += dz * 8;

		if (curZ < 0 && dz < 0) //roll back
		{
			dz = std::max(dz, -1.0);
			if (std::abs(dz)>1e-10) //rollback horizontal
				dist -= curZ / dz;
			break;
		}
		dz -= (double)(50 * weight / strength)/100;
		if (dz <= -2.0) //become falling
			break;
	}
	return dist;
}

/**
 * Set the origin voxel, used for the blaster launcher.
 * @param pos the origin voxel.
 */
void ProjectileFlyBState::setOriginVoxel(const Position& pos)
{
	_originVoxel = pos;
}

/**
 * Set the boolean flag to angle a blaster bomb towards the floor.
 */
void ProjectileFlyBState::targetFloor()
{
	_targetFloor = true;
}

void ProjectileFlyBState::projectileHitUnit(Position pos)
{
	BattleUnit *victim = _parent->getSave()->getTile(pos.toTile())->getOverlappingUnit(_parent->getSave());
	BattleUnit *targetVictim = _parent->getSave()->getTile(_action.target)->getUnit(); // Who we were aiming at (not necessarily who we hit)
	if (victim && !victim->isOut())
	{
		victim->getStatistics()->hitCounter++;
		if (_unit->getOriginalFaction() == FACTION_PLAYER && victim->getOriginalFaction() == FACTION_PLAYER)
		{
			victim->getStatistics()->shotByFriendlyCounter++;
			_unit->getStatistics()->shotFriendlyCounter++;
		}
		if (victim == targetVictim) // Hit our target
		{
			int distanceSq = _action.actor->distance3dToUnitSq(victim);
			int distance = (int)std::ceil(sqrt(float(distanceSq)));
			int accuracy = BattleUnit::getFiringAccuracy(BattleActionAttack::GetAferShoot(_action, _ammo), _parent->getMod());

			{
				int upperLimit, lowerLimit;
				int dropoff = _action.weapon->getRules()->calculateLimits(upperLimit, lowerLimit, _parent->getSave()->getDepth(), _action.type);

				if (distance > upperLimit)
				{
					accuracy -= (distance - upperLimit) * dropoff;
				}
				else if (distance < lowerLimit)
				{
					accuracy -= (lowerLimit - distance) * dropoff;
				}
				if (accuracy < 0)
				{
					accuracy = 0;
				}
			}

			_unit->getStatistics()->shotsLandedCounter++;
			if (distance > 30)
			{
				_unit->getStatistics()->longDistanceHitCounter++;
			}
			if (accuracy < distance)
			{
				_unit->getStatistics()->lowAccuracyHitCounter++;
			}
		}
		int turnBefore = victim->getTurnsSinceSeen(_unit->getFaction());
		victim->updateEnemyKnowledge(_parent->getSave()->getTileIndex(victim->getPosition()), true);
		if (turnBefore != victim->getTurnsSinceSeen(_unit->getFaction()))
		{
			for (BattleUnit* unit : *(_parent->getSave()->getUnits()))
			{
				if (unit->isOut())
					continue;
				if (!unit->getAIModule() || !unit->isBrutal())
					continue;
				unit->checkForReactivation(_parent->getSave());
			}
		}
	}
}

}
