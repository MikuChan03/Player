/*
 * This file is part of EasyRPG Player.
 *
 * EasyRPG Player is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * EasyRPG Player is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with EasyRPG Player. If not, see <http://www.gnu.org/licenses/>.
 */

// Headers
#include <algorithm>
#include <sstream>
#include <ciso646>

#include "rpg_battlecommand.h"
#include "input.h"
#include "output.h"
#include "player.h"
#include "sprite.h"
#include "graphics.h"
#include "filefinder.h"
#include "cache.h"
#include "game_battlealgorithm.h"
#include "game_message.h"
#include "game_system.h"
#include "game_temp.h"
#include "game_party.h"
#include "game_enemy.h"
#include "game_enemyparty.h"
#include "game_switches.h"
#include "game_battle.h"
#include "battle_animation.h"
#include "scene_battle.h"
#include "scene_battle_rpg2k.h"
#include "scene_battle_rpg2k3.h"
#include "bitmap.h"

Scene_Battle::Scene_Battle() :
	actor_index(0),
	active_actor(NULL),
	skill_item(NULL)
{
	Scene::type = Scene::Battle;
}

Scene_Battle::~Scene_Battle() {
	Game_Battle::Quit();
}

void Scene_Battle::Start() {
	if (Player::battle_test_flag) {
		Game_Temp::battle_troop_id = Player::battle_test_troop_id;
	}

	if (Game_Temp::battle_troop_id <= 0 ||
		Game_Temp::battle_troop_id > (int)Data::troops.size()) {
			Output::Warning("Invalid Monster Party Id %d", Game_Temp::battle_troop_id);
			Game_Temp::battle_result = Game_Temp::BattleVictory;
			Scene::Pop();
			return;
	}

	if (Player::battle_test_flag) {
		InitBattleTest();
	} else {
		Main_Data::game_enemyparty.reset(new Game_EnemyParty());
		Main_Data::game_enemyparty->Setup(Game_Temp::battle_troop_id);
	}

	Game_Battle::Init();

	cycle = 0;
	auto_battle = false;
	enemy_action = NULL;

	CreateCursors();
	CreateWindows();

	Game_Temp::map_bgm = NULL; // Play map BGM on Scene_Map return
	Game_System::BgmPlay(Data::system.battle_music);

	Game_System::SePlay(Data::system.battle_se);

	if (!Game_Temp::battle_background.empty())
		background.reset(new Background(Game_Temp::battle_background));
	else
		background.reset(new Background(Game_Temp::battle_terrain_id));

	SetState(State_Start);
}

void Scene_Battle::CreateCursors() {

}

void Scene_Battle::CreateWindows() {
	CreateBattleOptionWindow();
	CreateBattleTargetWindow();
	CreateBattleCommandWindow();
	CreateBattleMessageWindow();

	help_window.reset(new Window_Help(0, 0, 320, 32));
	help_window->SetVisible(false);

	item_window.reset(new Window_Item(0, 160, 320, 80));
	item_window->SetHelpWindow(help_window.get());
	item_window->Refresh();
	item_window->SetIndex(0);

	skill_window.reset(new Window_Skill(0, 160, 320, 80));
	skill_window->SetHelpWindow(help_window.get());

	status_window.reset(new Window_BattleStatus(0, 160, 320 - 76, 80));
}

void Scene_Battle::Update() {
	options_window->Update();
	status_window->Update();
	command_window->Update();
	help_window->Update();
	item_window->Update();
	skill_window->Update();
	target_window->Update();
	message_window->Update();

	if (!message_window->GetVisible()) {
		ProcessActions();
	}

	if (!Game_Message::message_waiting) {
		ProcessInput();
	}

	//DoAuto();

	UpdateBackground();
	//UpdateCursors();
	//UpdateSprites();
	//UpdateFloaters();
	//UpdateAnimations();

	Game_Battle::Update();

	Main_Data::game_screen->Update();
}

void Scene_Battle::InitBattleTest()
{
	Game_Temp::battle_troop_id = Player::battle_test_troop_id;
	Game_Temp::battle_background = Data::system.battletest_background;

	Main_Data::game_party->SetupBattleTestMembers();

	Main_Data::game_enemyparty.reset(new Game_EnemyParty());
	Main_Data::game_enemyparty->Setup(Game_Temp::battle_troop_id);
}

void Scene_Battle::NextTurn() {
	auto_battle = false;

	Game_Battle::NextTurn();
	Game_Battle::UpdateEvents();
}

void Scene_Battle::SetAnimationState(Game_Battler* target, int new_state) {
	Sprite_Battler* target_sprite = Game_Battle::GetSpriteset().FindBattler(target);
	if (target_sprite) {
		target_sprite->SetAnimationState(new_state);
	}
}

void Scene_Battle::UpdateBackground() {
	if (Game_Temp::battle_background != Game_Battle::background_name) {
		Game_Temp::battle_background = Game_Battle::background_name;
		if (!Game_Temp::battle_background.empty()) {
			background.reset(new Background(Game_Temp::battle_background));
		}
	}
}

EASYRPG_SHARED_PTR<Scene_Battle> Scene_Battle::Create()
{
	if (Player::engine == Player::EngineRpg2k) {
		return EASYRPG_MAKE_SHARED<Scene_Battle_Rpg2k>();
	}
	else {
		return EASYRPG_MAKE_SHARED<Scene_Battle_Rpg2k3>();
	}
}

void Scene_Battle::CreateEnemyAction(Game_Enemy* enemy, const RPG::EnemyAction* action) {
	switch (action->kind) {
	case RPG::EnemyAction::Kind_basic:
		CreateEnemyActionBasic(enemy, action);
		break;
	case RPG::EnemyAction::Kind_skill:
		CreateEnemyActionSkill(enemy, action);
		return;
		break;
	case RPG::EnemyAction::Kind_transformation:
		// ToDo
		enemy->SetBattleAlgorithm(EASYRPG_MAKE_SHARED<Game_BattleAlgorithm::Transform>(enemy, action->enemy_id));
		battle_actions.push_back(enemy);
	}
}

void Scene_Battle::CreateEnemyActionBasic(Game_Enemy* enemy, const RPG::EnemyAction* action) {
	if (action->kind != RPG::EnemyAction::Kind_basic) {
		return;
	}

	switch (action->basic) {
		case RPG::EnemyAction::Basic_attack:
			enemy->SetBattleAlgorithm(EASYRPG_MAKE_SHARED<Game_BattleAlgorithm::Normal>(enemy, Main_Data::game_party->GetRandomAliveBattler()));
			break;
		case RPG::EnemyAction::Basic_dual_attack:
			enemy->SetBattleAlgorithm(EASYRPG_MAKE_SHARED<Game_BattleAlgorithm::NormalDual>(enemy, Main_Data::game_party->GetRandomAliveBattler()));
			break;
		case RPG::EnemyAction::Basic_defense:
			enemy->SetBattleAlgorithm(EASYRPG_MAKE_SHARED<Game_BattleAlgorithm::Defend>(enemy));
			break;
		case RPG::EnemyAction::Basic_observe:
			enemy->SetBattleAlgorithm(EASYRPG_MAKE_SHARED<Game_BattleAlgorithm::Observe>(enemy));
			break;
		case RPG::EnemyAction::Basic_charge:
			enemy->SetBattleAlgorithm(EASYRPG_MAKE_SHARED<Game_BattleAlgorithm::Charge>(enemy));
			break;
		case RPG::EnemyAction::Basic_autodestruction:
			enemy->SetBattleAlgorithm(EASYRPG_MAKE_SHARED<Game_BattleAlgorithm::SelfDestruct>(enemy));
			break;
		case RPG::EnemyAction::Basic_escape:
			enemy->SetBattleAlgorithm(EASYRPG_MAKE_SHARED<Game_BattleAlgorithm::Escape>(enemy));
			break;
		case RPG::EnemyAction::Basic_nothing:
			// no-op
			break;
	}

	if (action->basic != RPG::EnemyAction::Basic_nothing) {
		battle_actions.push_back(enemy);
	}
}

void Scene_Battle::RemoveCurrentAction() {
	battle_actions.front()->SetBattleAlgorithm(NULL);
	battle_actions.pop_front();
}

void Scene_Battle::CreateEnemyActionSkill(Game_Enemy* enemy, const RPG::EnemyAction* action) {
	if (action->kind != RPG::EnemyAction::Kind_skill) {
		return;
	}

	const RPG::Skill& skill = Data::skills[action->skill_id - 1];

	switch (skill.type) {
	case RPG::Skill::Type_teleport:
	case RPG::Skill::Type_escape:
	case RPG::Skill::Type_switch:
		//BeginSkill();
		return;
	case RPG::Skill::Type_normal:
	default:
		break;
	}

	switch (skill.scope) {
	case RPG::Skill::Scope_enemy:
		// ToDo
		break;
	case RPG::Skill::Scope_ally:
		// ToDo
		break;
	case RPG::Skill::Scope_enemies:
		enemy->SetBattleAlgorithm(EASYRPG_MAKE_SHARED<Game_BattleAlgorithm::Skill>(enemy, Main_Data::game_party.get(), skill));
		battle_actions.push_back(enemy);
		SetState(State_SelectActor);
		break;
	case RPG::Skill::Scope_self:
		enemy->SetBattleAlgorithm(EASYRPG_MAKE_SHARED<Game_BattleAlgorithm::Skill>(enemy, enemy, skill));
		battle_actions.push_back(enemy);
		SetState(State_SelectActor);
		break;
	case RPG::Skill::Scope_party: {
									  enemy->SetBattleAlgorithm(EASYRPG_MAKE_SHARED<Game_BattleAlgorithm::Skill>(enemy, Main_Data::game_enemyparty.get(), *skill_window->GetSkill()));
									  battle_actions.push_back(enemy);
									  SetState(State_SelectActor);
									  break;
	}
	}
}
