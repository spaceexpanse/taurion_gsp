/*
    GSP for the Taurion blockchain game
    Copyright (C) 2020-2021  Autonomous Worlds Ltd

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#include "building.hpp"

#include <glog/logging.h>

namespace pxd
{

Building::Building (Database& d, const std::string& t,
                    const std::string& o, const Faction f)
  : CombatEntity(d), id(db.GetNextId ()),
    tracker(db.TrackHandle ("building", id)),
    type(t), owner(o), faction(f),
    pos(0, 0), dirtyFields(true)
{
  VLOG (1)
      << "Created new building with ID " << id << ": "
      << "type=" << type << ", owner=" << owner;
  effects.SetToDefault ();
  data.SetToDefault ();

  if (f == Faction::ANCIENT)
    CHECK_EQ (owner, "");
}

Building::Building (Database& d, const Database::Result<BuildingResult>& res)
  : CombatEntity(d, res), dirtyFields(false)
{
  id = res.Get<BuildingResult::id> ();
  tracker = db.TrackHandle ("building", id);

  type = res.Get<BuildingResult::type> ();
  faction = GetFactionFromColumn (res);
  if (faction != Faction::ANCIENT)
    owner = res.Get<BuildingResult::owner> ();
  pos = GetCoordFromColumn (res);

  if (res.IsNull<BuildingResult::effects> ())
    effects.SetToDefault ();
  else
    effects = res.GetProto<BuildingResult::effects> ();

  data = res.GetProto<BuildingResult::proto> ();

  VLOG (2) << "Fetched building with ID " << id << " from database result";
}

Building::~Building ()
{
  /* For now, we only implement "full update".  For buildings, fields are
     not modified that often, so it seems not like a useful optimisation
     to specifically handle that case.  */

  if (isNew || CombatEntity::IsDirtyFull () || CombatEntity::IsDirtyFields ()
        || dirtyFields || effects.IsDirty () || data.IsDirty ())
    {
      VLOG (2)
          << "Building " << id << " has been modified, updating DB";

      auto stmt = db.Prepare (R"(
        INSERT OR REPLACE INTO `buildings`
          (`id`, `type`,
           `faction`, `owner`, `x`, `y`,
           `hp`, `regendata`, `target`, `friendlytargets`,
           `attackrange`, `friendlyrange`, `canregen`,
           `effects`, `proto`)
          VALUES
          (?1, ?2,
           ?3, ?4, ?5, ?6,
           ?7, ?8, ?9, ?10,
           ?11, ?12, ?13,
           ?14, ?15)
      )");

      stmt.Bind (1, id);
      stmt.Bind (2, type);
      BindFactionParameter (stmt, 3, faction);
      if (faction == Faction::ANCIENT)
        stmt.BindNull (4);
      else
        stmt.Bind (4, owner);
      BindCoordParameter (stmt, 5, 6, pos);
      CombatEntity::BindFields (stmt, 7, 10, 13);
      CombatEntity::BindFullFields (stmt, 8, 9, 11, 12);

      if (effects.IsEmpty ())
        stmt.BindNull (14);
      else
        stmt.BindProto (14, effects);

      stmt.BindProto (15, data);
      stmt.Execute ();

      return;
    }

  VLOG (2) << "Building " << id << " is not dirty, no update";
}

const std::string&
Building::GetOwner () const
{
  CHECK (faction != Faction::ANCIENT) << "Ancient building has no owner";
  return owner;
}

void
Building::SetOwner (const std::string& o)
{
  CHECK (faction != Faction::ANCIENT) << "Ancient building has no owner";
  dirtyFields = true;
  owner = o;
}

void
Building::SetCentre (const HexCoord& c)
{
  CHECK (isNew) << "Only new building can have its centre set";
  pos = c;
}

proto::TargetId
Building::GetIdAsTarget () const
{
  proto::TargetId res;
  res.set_type (proto::TargetId::TYPE_BUILDING);
  res.set_id (id);
  return res;
}

BuildingsTable::Handle
BuildingsTable::CreateNew (const std::string& type,
                           const std::string& owner, const Faction faction)
{
  return Handle (new Building (db, type, owner, faction));
}

BuildingsTable::Handle
BuildingsTable::GetFromResult (const Database::Result<BuildingResult>& res)
{
  return Handle (new Building (db, res));
}

BuildingsTable::Handle
BuildingsTable::GetById (const Database::IdT id)
{
  auto stmt = db.Prepare ("SELECT * FROM `buildings` WHERE `id` = ?1");
  stmt.Bind (1, id);
  auto res = stmt.Query<BuildingResult> ();
  if (!res.Step ())
    return nullptr;

  auto c = GetFromResult (res);
  CHECK (!res.Step ());
  return c;
}

Database::Result<BuildingResult>
BuildingsTable::QueryAll ()
{
  auto stmt = db.Prepare ("SELECT * FROM `buildings` ORDER BY `id`");
  return stmt.Query<BuildingResult> ();
}

void
BuildingsTable::DeleteById (const Database::IdT id)
{
  VLOG (1) << "Deleting building with ID " << id;

  auto stmt = db.Prepare (R"(
    DELETE FROM `buildings`
      WHERE `id` = ?1
  )");
  stmt.Bind (1, id);
  stmt.Execute ();
}

Database::Result<BuildingResult>
BuildingsTable::QueryWithAttacks ()
{
  auto stmt = db.Prepare (R"(
    SELECT *
      FROM `buildings`
      WHERE (`attackrange` IS NOT NULL) OR (`friendlyrange` IS NOT NULL)
      ORDER BY `id`
  )");
  return stmt.Query<BuildingResult> ();
}

Database::Result<BuildingResult>
BuildingsTable::QueryForRegen ()
{
  auto stmt = db.Prepare (R"(
    SELECT *
      FROM `buildings`
      WHERE `canregen`
      ORDER BY `id`
  )");
  return stmt.Query<BuildingResult> ();
}

Database::Result<BuildingResult>
BuildingsTable::QueryWithTarget ()
{
  auto stmt = db.Prepare (R"(
    SELECT *
      FROM `buildings`
      WHERE (`target` IS NOT NULL) OR `friendlytargets`
      ORDER BY `id`
  )");
  return stmt.Query<BuildingResult> ();
}

void
BuildingsTable::ClearAllEffects ()
{
  VLOG (1) << "Clearing all combat effects on buildings";

  auto stmt = db.Prepare (R"(
    UPDATE `buildings`
      SET `effects` = NULL
      WHERE `effects` IS NOT NULL
  )");
  stmt.Execute ();
}

} // namespace pxd
