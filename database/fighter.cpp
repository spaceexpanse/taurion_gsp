/*
    GSP for the Taurion blockchain game
    Copyright (C) 2019-2020  Autonomous Worlds Ltd

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

#include "fighter.hpp"

#include <glog/logging.h>

namespace pxd
{

proto::TargetId
Fighter::GetId () const
{
  proto::TargetId res;

  if (building != nullptr)
    {
      res.set_type (proto::TargetId::TYPE_BUILDING);
      res.set_id (building->GetId ());
    }
  else
    {
      CHECK (character != nullptr);
      res.set_type (proto::TargetId::TYPE_CHARACTER);
      res.set_id (character->GetId ());
    }

  return res;
}

Faction
Fighter::GetFaction () const
{
  if (building != nullptr)
    return building->GetFaction ();

  CHECK (character != nullptr);
  return character->GetFaction ();
}

const HexCoord&
Fighter::GetPosition () const
{
  if (building != nullptr)
    return building->GetCentre ();

  CHECK (character != nullptr);
  return character->GetPosition ();
}

const proto::RegenData&
Fighter::GetRegenData () const
{
  if (building != nullptr)
    return building->GetRegenData ();

  CHECK (character != nullptr);
  return character->GetRegenData ();
}

const proto::CombatData&
Fighter::GetCombatData () const
{
  if (building != nullptr)
    {
      const auto& pb = building->GetProto ();

      /* Every fighter must have combat data to be valid.  This is set when
         first created and then only updated.  Enforce this requirement here,
         so that we do not accidentally work with an empty proto just because it
         has not been initialised due to a bug.  */
      CHECK (pb.has_combat_data ());

      return pb.combat_data ();
    }

  CHECK (character != nullptr);
  const auto& pb = character->GetProto ();
  CHECK (pb.has_combat_data ());
  return pb.combat_data ();
}

HexCoord::IntT
Fighter::GetAttackRange () const
{
  if (building != nullptr)
    return building->GetAttackRange ();

  CHECK (character != nullptr);
  return character->GetAttackRange ();
}

const proto::TargetId&
Fighter::GetTarget () const
{
  const proto::TargetId* pb;
  if (building != nullptr)
    pb = &building->GetTarget ();
  else
    {
      CHECK (character != nullptr);
      pb = &character->GetTarget ();
    }

  CHECK (pb->has_id ());
  return *pb;
}

void
Fighter::SetTarget (const proto::TargetId& target)
{
  CHECK (target.has_id ());

  if (building != nullptr)
    building->MutableTarget () = target;
  else
    {
      CHECK (character != nullptr);
      character->MutableTarget () = target;
    }
}

void
Fighter::ClearTarget ()
{
  if (building != nullptr)
    building->MutableTarget ().clear_id ();
  else
    {
      CHECK (character != nullptr);
      character->MutableTarget ().clear_id ();
    }
}

const proto::HP&
Fighter::GetHP () const
{
  if (building != nullptr)
    return building->GetHP ();

  CHECK (character != nullptr);
  return character->GetHP ();
}

proto::HP&
Fighter::MutableHP ()
{
  if (building != nullptr)
    return building->MutableHP ();

  CHECK (character != nullptr);
  return character->MutableHP ();
}

void
Fighter::reset ()
{
  building.reset ();
  character.reset ();
}

bool
Fighter::empty () const
{
  return building == nullptr && character == nullptr;
}

Fighter
FighterTable::GetForTarget (const proto::TargetId& id)
{
  switch (id.type ())
    {
    case proto::TargetId::TYPE_BUILDING:
      return Fighter (buildings.GetById (id.id ()));

    case proto::TargetId::TYPE_CHARACTER:
      return Fighter (characters.GetById (id.id ()));

    default:
      LOG (FATAL) << "Invalid target type: " << static_cast<int> (id.type ());
    }
}

void
FighterTable::ProcessWithAttacks (const Callback& cb)
{
  {
    auto res = buildings.QueryWithAttacks ();
    while (res.Step ())
      cb (Fighter (buildings.GetFromResult (res)));
  }

  {
    auto res = characters.QueryWithAttacks ();
    while (res.Step ())
      cb (Fighter (characters.GetFromResult (res)));
  }
}

void
FighterTable::ProcessForRegen (const Callback& cb)
{
  {
    auto res = buildings.QueryForRegen ();
    while (res.Step ())
      cb (Fighter (buildings.GetFromResult (res)));
  }

  {
    auto res = characters.QueryForRegen ();
    while (res.Step ())
      cb (Fighter (characters.GetFromResult (res)));
  }
}

void
FighterTable::ProcessWithTarget (const Callback& cb)
{
  {
    auto res = buildings.QueryWithTarget ();
    while (res.Step ())
      cb (Fighter (buildings.GetFromResult (res)));
  }

  {
    auto res = characters.QueryWithTarget ();
    while (res.Step ())
      cb (Fighter (characters.GetFromResult (res)));
  }
}

} // namespace pxd
