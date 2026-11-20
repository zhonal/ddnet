/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include "controls.h"

#include <base/dbg.h>
#include <base/math.h>
#include <base/mem.h>
#include <base/time.h>
#include <base/vmath.h>

#include <engine/client.h>
#include <engine/shared/config.h>

#include <generated/protocol.h>

#include <game/client/components/camera.h>
#include <game/client/components/chat.h>
#include <game/client/components/menus.h>
#include <game/client/components/scoreboard.h>
#include <game/client/gameclient.h>
#include <game/collision.h>
#include <game/gamecore.h>
#include <game/localization.h>
#include <game/mapitems.h>

#include <algorithm>
#include <cmath>

namespace {

constexpr int AVOID_LOOKAHEAD_TICKS = 30;
constexpr int AVOID_HOOK_SIM_TICKS = 32;
constexpr int AVOID_RAYS = 32;
constexpr int AVOID_HOOK_WAIT_TICKS = 4; // насколько заранее нужно знать, что хук ещё успеет спасти
constexpr float AVOID_HOOK_START = CCharacterCore::PhysicalSize() * 1.5f;
constexpr float AVOID_HOOK_NEAR = 46.0f;
constexpr float AVOID_TILE_SIZE = 32.0f;
constexpr int AVOID_MAX_HOLD_SECONDS = 3;

// Физика. Значения по умолчанию -- стандартные для DDNet (по памяти, не измерены на твоей сборке).
// ReadTune заменяет их текущими значениями сервера, если параметр с таким именем найден.
struct SAvoidTune
{
	float m_Gravity = 0.5f;
	float m_GroundSpeed = 10.0f;
	float m_GroundAccel = 2.0f;
	float m_GroundFriction = 0.5f;
	float m_AirSpeed = 5.0f;
	float m_AirAccel = 1.5f;
	float m_AirFriction = 0.95f;
	float m_GroundJump = 13.2f;
	float m_AirJump = 12.0f;
	float m_HookLength = 380.0f;
	float m_HookFireSpeed = 80.0f;
	float m_HookDragAccel = 3.0f;
	float m_HookDragSpeed = 15.0f;
};

float TuneOr(const CTuningParams &Tuning, const char *pName, float Default)
{
	float Value = 0.0f;
	return Tuning.Get(pName, &Value) ? Value : Default;
}

SAvoidTune ReadTune(const CTuningParams &Tuning)
{
	SAvoidTune T;
	T.m_Gravity = TuneOr(Tuning, "gravity", T.m_Gravity);
	T.m_GroundSpeed = TuneOr(Tuning, "ground_control_speed", T.m_GroundSpeed);
	T.m_GroundAccel = TuneOr(Tuning, "ground_control_accel", T.m_GroundAccel);
	T.m_GroundFriction = TuneOr(Tuning, "ground_friction", T.m_GroundFriction);
	T.m_AirSpeed = TuneOr(Tuning, "air_control_speed", T.m_AirSpeed);
	T.m_AirAccel = TuneOr(Tuning, "air_control_accel", T.m_AirAccel);
	T.m_AirFriction = TuneOr(Tuning, "air_friction", T.m_AirFriction);
	T.m_GroundJump = TuneOr(Tuning, "ground_jump_impulse", T.m_GroundJump);
	T.m_AirJump = TuneOr(Tuning, "air_jump_impulse", T.m_AirJump);
	T.m_HookLength = TuneOr(Tuning, "hook_length", T.m_HookLength);
	T.m_HookFireSpeed = TuneOr(Tuning, "hook_fire_speed", T.m_HookFireSpeed);
	T.m_HookDragAccel = TuneOr(Tuning, "hook_drag_accel", T.m_HookDragAccel);
	T.m_HookDragSpeed = TuneOr(Tuning, "hook_drag_speed", T.m_HookDragSpeed);
	return T;
}

bool IsDangerTile(int Tile)
{
	return Tile == TILE_DEATH || Tile == TILE_FREEZE || Tile == TILE_DFREEZE;
}

bool IsDangerPoint(const CCollision *pCollision, vec2 Pos)
{
	const int Index = pCollision->GetPureMapIndex(Pos);
	if(IsDangerTile(pCollision->GetTileIndex(Index)) || IsDangerTile(pCollision->GetFrontTileIndex(Index)))
		return true;
	return pCollision->IsTeleport(Index) != 0 || pCollision->IsEvilTeleport(Index) != 0;
}

// Тия попадает во фриз, когда в опасной клетке оказывается центр или один из четырёх «углов» тела.
bool IsDangerBox(const CCollision *pCollision, vec2 Pos)
{
	const float R = CCharacterCore::PhysicalSize() / 3.0f;
	return IsDangerPoint(pCollision, Pos) ||
	       IsDangerPoint(pCollision, Pos + vec2(-R, 0.0f)) ||
	       IsDangerPoint(pCollision, Pos + vec2(R, 0.0f)) ||
	       IsDangerPoint(pCollision, Pos + vec2(0.0f, -R)) ||
	       IsDangerPoint(pCollision, Pos + vec2(0.0f, R));
}

float SatAdd(float Current, float Limit, float Modifier)
{
	if(Modifier < 0.0f)
	{
		if(Current < -Limit)
			return Current;
		Current += Modifier;
		return Current < -Limit ? -Limit : Current;
	}
	if(Current > Limit)
		return Current;
	Current += Modifier;
	return Current > Limit ? Limit : Current;
}

// Один тик движения без хука; Dir = -1, 0 или 1 (удерживаемое направление).
void SimStep(const CCollision *pCollision, const SAvoidTune &T, vec2 *pPos, vec2 *pVel, int Dir)
{
	const bool Grounded = pCollision->IsOnGround(*pPos, CCharacterCore::PhysicalSize());
	pVel->y += T.m_Gravity;
	const float MaxSpeed = Grounded ? T.m_GroundSpeed : T.m_AirSpeed;
	const float Accel = Grounded ? T.m_GroundAccel : T.m_AirAccel;
	const float Friction = Grounded ? T.m_GroundFriction : T.m_AirFriction;
	if(Dir < 0)
		pVel->x = SatAdd(pVel->x, MaxSpeed, -Accel);
	else if(Dir > 0)
		pVel->x = SatAdd(pVel->x, MaxSpeed, Accel);
	else
		pVel->x *= Friction;
	pCollision->MoveBox(pPos, pVel, CCharacterCore::PhysicalSizeVec2(), vec2(0.0f, 0.0f));
}

// Тик, на котором тия впервые окажется в опасной клетке, или -1, если за Ticks тиков этого не будет.
// Если MaxDist больше нуля, опасность дальше MaxDist пикселей от начальной позиции в расчёт не берётся.
int SimDangerTick(const CCollision *pCollision, const SAvoidTune &T, vec2 Pos, vec2 Vel, int Dir, int Ticks, float MaxDist = 0.0f)
{
	const vec2 Start = Pos;
	for(int Tick = 1; Tick <= Ticks; Tick++)
	{
		SimStep(pCollision, T, &Pos, &Vel, Dir);
		if(IsDangerBox(pCollision, Pos))
			return (MaxDist <= 0.0f || distance(Start, Pos) <= MaxDist) ? Tick : -1;
	}
	return -1;
}

int SimJumpDangerTick(const CCollision *pCollision, const SAvoidTune &T, vec2 Pos, vec2 Vel, float Impulse, int Dir, int Ticks, float MaxDist = 0.0f)
{
	Vel.y = -Impulse;
	return SimDangerTick(pCollision, T, Pos, Vel, Dir, Ticks, MaxDist);
}

int HookFlightTicks(const SAvoidTune &T, float Dist)
{
	const float Speed = T.m_HookFireSpeed > 1.0f ? T.m_HookFireSpeed : 1.0f;
	const float Flight = std::ceil((Dist - AVOID_HOOK_START) / Speed);
	return (Flight > 0.0f ? (int)Flight : 0) + 1;
}

// Тия цепляется за HookPoint через FlightTicks тиков и подтягивается к нему.
int SimHookDangerTick(const CCollision *pCollision, const SAvoidTune &T, vec2 Pos, vec2 Vel, vec2 HookPoint, int FlightTicks, int Dir, int Ticks, float MaxDist = 0.0f)
{
	const vec2 Start = Pos;
	for(int Tick = 1; Tick <= Ticks; Tick++)
	{
		if(Tick > FlightTicks && distance(HookPoint, Pos) > AVOID_HOOK_NEAR)
		{
			vec2 HookVel = normalize(HookPoint - Pos) * T.m_HookDragAccel;
			if(HookVel.y > 0.0f)
				HookVel.y *= 0.3f;
			if((HookVel.x < 0.0f && Dir < 0) || (HookVel.x > 0.0f && Dir > 0))
				HookVel.x *= 0.95f;
			else
				HookVel.x *= 0.75f;
			const vec2 NewVel = Vel + HookVel;
			if(length(NewVel) < T.m_HookDragSpeed || length(NewVel) < length(Vel))
				Vel = NewVel;
		}
		SimStep(pCollision, T, &Pos, &Vel, Dir);
		if(IsDangerBox(pCollision, Pos))
			return (MaxDist <= 0.0f || distance(Start, Pos) <= MaxDist) ? Tick : -1;
	}
	return -1;
}

// Куда попадёт хук, выпущенный из Pos в направлении Dir. Ложь, если хукабельной стены нет.
bool RayHook(const CCollision *pCollision, const SAvoidTune &T, vec2 Pos, vec2 Dir, vec2 *pHit)
{
	vec2 Hit, Before;
	int Tele = 0;
	const int Tile = pCollision->IntersectLineTeleHook(Pos + Dir * AVOID_HOOK_START, Pos + Dir * T.m_HookLength, &Hit, &Before, &Tele);
	if(Tile == 0 || Tile == TILE_NOHOOK || Tile == TILE_TELEINHOOK)
		return false;
	*pHit = Hit;
	return true;
}

// Ближайшая стена, за которую можно зацепиться и не попасть в опасность. Если полностью безопасной
// нет и SafeOnly выключен, берёт ту, что оттягивает опасность дальше всего (но заметно лучше, чем без хука).
bool FindHookTarget(const CCollision *pCollision, const SAvoidTune &T, vec2 Pos, vec2 Vel, int Dir, int BaselineTick, bool SafeOnly, vec2 *pTarget)
{
	bool Found = false;
	bool BestSafe = false;
	float BestDist = 0.0f;
	int BestTick = -1;
	for(int i = 0; i < AVOID_RAYS; i++)
	{
		const float Angle = (float)i * (2.0f * pi / (float)AVOID_RAYS);
		vec2 Hit;
		if(!RayHook(pCollision, T, Pos, vec2(cosf(Angle), sinf(Angle)), &Hit))
			continue;
		const float Dist = distance(Pos, Hit);
		const int Tick = SimHookDangerTick(pCollision, T, Pos, Vel, Hit, HookFlightTicks(T, Dist), Dir, AVOID_HOOK_SIM_TICKS);
		const bool Safe = Tick < 0;
		if(!Safe && (SafeOnly || Tick <= BaselineTick + 2))
			continue;

		bool Take = false;
		if(!Found)
			Take = true;
		else if(Safe && !BestSafe)
			Take = true;
		else if(Safe && BestSafe)
			Take = Dist < BestDist;
		else if(!Safe && !BestSafe)
			Take = Tick > BestTick;
		if(Take)
		{
			Found = true;
			BestSafe = Safe;
			BestDist = Dist;
			BestTick = Tick;
			*pTarget = Hit;
		}
	}
	return Found;
}

// Тик опасности при текущем вводе. Если игрок держит зацепленный хук, учитывается его подтяжка.
int BaselineDangerTick(const CCollision *pCollision, const SAvoidTune &T, vec2 Pos, vec2 Vel, int Dir, bool HookPulling, vec2 HookPoint, float MaxDist)
{
	if(HookPulling)
		return SimHookDangerTick(pCollision, T, Pos, Vel, HookPoint, 0, Dir, AVOID_LOOKAHEAD_TICKS, MaxDist);
	return SimDangerTick(pCollision, T, Pos, Vel, Dir, AVOID_LOOKAHEAD_TICKS, MaxDist);
}

// Можно ли ещё немного подождать: через AVOID_HOOK_WAIT_TICKS тиков хук всё ещё будет безопасно спасать.
bool HookCanWait(const CCollision *pCollision, const SAvoidTune &T, vec2 Pos, vec2 Vel, int Dir)
{
	for(int i = 0; i < AVOID_HOOK_WAIT_TICKS; i++)
	{
		SimStep(pCollision, T, &Pos, &Vel, Dir);
		if(IsDangerBox(pCollision, Pos))
			return false;
	}
	vec2 Target;
	return FindHookTarget(pCollision, T, Pos, Vel, Dir, -1, true, &Target);
}

struct SAvoidPlan
{
	int m_Direction = 0;
	bool m_Jump = false;
	bool m_Hook = false; // выпустить хук в m_HookTarget (или держать уже выпущенный)
	bool m_Release = false; // отпустить хук, который игрок держит сам
	vec2 m_HookTarget = vec2(0.0f, 0.0f);
};

// Возвращает true, если нужно вмешаться. Фриз дальше радиуса ReactDist игнорируется, кроме одного случая:
// хук в воздухе начинается заранее, ровно когда подождать ещё 4 тика уже нельзя.
// Порядок спасения: на земле ходьба, прыжок, хук; в воздухе ходьба, хук, прыжок.
bool PlanAvoid(const CCollision *pCollision, const SAvoidTune &T, vec2 Pos, vec2 Vel, int UserDir, bool CanJump, bool AllowWalk, bool AllowJump, bool AllowHook, float ReactDist, bool HookPulling, vec2 HookPoint, SAvoidPlan *pPlan)
{
	*pPlan = SAvoidPlan();
	pPlan->m_Direction = UserDir;
	const int BaseFar = BaselineDangerTick(pCollision, T, Pos, Vel, UserDir, HookPulling, HookPoint, 0.0f);
	if(BaseFar < 0)
		return false;
	const int BaseNear = BaselineDangerTick(pCollision, T, Pos, Vel, UserDir, HookPulling, HookPoint, ReactDist);
	const bool Grounded = pCollision->IsOnGround(Pos, CCharacterCore::PhysicalSize());

	if(BaseNear < 0)
	{
		if(Grounded || !AllowHook || HookPulling)
			return false;
		vec2 Target;
		if(!FindHookTarget(pCollision, T, Pos, Vel, UserDir, BaseFar, true, &Target) || HookCanWait(pCollision, T, Pos, Vel, UserDir))
			return false;
		pPlan->m_Hook = true;
		pPlan->m_HookTarget = Target;
		return true;
	}

	// Игрок тянется своим хуком к опасности: сначала пробуем просто отпустить.
	if(HookPulling && SimDangerTick(pCollision, T, Pos, Vel, UserDir, AVOID_LOOKAHEAD_TICKS, ReactDist) < 0)
	{
		pPlan->m_Release = true;
		return true;
	}

	auto TryWalk = [&]() {
		int aDirs[2];
		if(std::fabs(Vel.x) > 0.5f)
		{
			aDirs[0] = Vel.x > 0.0f ? -1 : 1; // против движения: так сопротивляемся толчку
			aDirs[1] = 0;
		}
		else if(UserDir != 0)
		{
			aDirs[0] = 0;
			aDirs[1] = -UserDir;
		}
		else
		{
			aDirs[0] = -1;
			aDirs[1] = 1;
		}
		for(int Dir : aDirs)
		{
			if(SimDangerTick(pCollision, T, Pos, Vel, Dir, AVOID_LOOKAHEAD_TICKS) < 0)
			{
				pPlan->m_Direction = Dir;
				pPlan->m_Release = HookPulling;
				return true;
			}
		}
		return false;
	};
	auto TryJump = [&]() {
		const float Impulse = Grounded ? T.m_GroundJump : T.m_AirJump;
		const int aDirs[3] = {UserDir, UserDir == 0 ? -1 : 0, UserDir == 0 ? 1 : -UserDir};
		for(int Dir : aDirs)
		{
			if(SimJumpDangerTick(pCollision, T, Pos, Vel, Impulse, Dir, AVOID_LOOKAHEAD_TICKS) < 0)
			{
				pPlan->m_Direction = Dir;
				pPlan->m_Jump = true;
				pPlan->m_Release = HookPulling;
				return true;
			}
		}
		return false;
	};
	auto TryHook = [&](bool SafeOnly) {
		if(!AllowHook || HookPulling)
			return false;
		vec2 Target;
		if(!FindHookTarget(pCollision, T, Pos, Vel, UserDir, BaseNear, SafeOnly, &Target))
			return false;
		pPlan->m_Hook = true;
		pPlan->m_HookTarget = Target;
		return true;
	};

	if(Grounded)
	{
		if((AllowWalk && TryWalk()) || (AllowJump && CanJump && TryJump()) || TryHook(false))
			return true;
	}
	else
	{
		if((AllowWalk && TryWalk()) || TryHook(true) || (AllowJump && CanJump && TryJump()) || TryHook(false))
			return true;
	}
	*pPlan = SAvoidPlan();
	pPlan->m_Direction = UserDir;
	return true;
}

} // namespace
CControls::CControls()
{
	mem_zero(&m_aLastData, sizeof(m_aLastData));
	std::fill(std::begin(m_aMouseOnAction), std::end(m_aMouseOnAction), false);
	std::fill(std::begin(m_aMousePos), std::end(m_aMousePos), vec2(0.0f, 0.0f));
	std::fill(std::begin(m_aMousePosOnAction), std::end(m_aMousePosOnAction), vec2(0.0f, 0.0f));
	std::fill(std::begin(m_aTargetPos), std::end(m_aTargetPos), vec2(0.0f, 0.0f));
	std::fill(std::begin(m_aMouseInputType), std::end(m_aMouseInputType), EMouseInputType::ABSOLUTE);
}

void CControls::OnReset()
{
	ResetInput(0);
	ResetInput(1);

	std::fill(std::begin(m_aAmmoCount), std::end(m_aAmmoCount), 0);

	m_LastSendTime = 0;
}

void CControls::ResetInput(int Dummy)
{
	m_aLastData[Dummy].m_Direction = 0;
	// simulate releasing the fire button
	if((m_aLastData[Dummy].m_Fire & 1) != 0)
		m_aLastData[Dummy].m_Fire++;
	m_aLastData[Dummy].m_Fire &= INPUT_STATE_MASK;
	m_aLastData[Dummy].m_Jump = 0;
	m_aInputData[Dummy] = m_aLastData[Dummy];

	m_aInputDirectionLeft[Dummy] = 0;
	m_aInputDirectionRight[Dummy] = 0;
}

void CControls::OnPlayerDeath()
{
	std::fill(std::begin(m_aAmmoCount), std::end(m_aAmmoCount), 0);
}

struct CInputState
{
	CControls *m_pControls;
	int *m_apVariables[NUM_DUMMIES];
	bool m_IsMouseAction;
};

void CControls::ConKeyInputState(IConsole::IResult *pResult, void *pUserData)
{
	CInputState *pState = (CInputState *)pUserData;

	if(pState->m_pControls->GameClient()->m_GameInfo.m_BugDDRaceInput && pState->m_pControls->GameClient()->m_Snap.m_SpecInfo.m_Active)
		return;

	const int Stroke = pResult->GetInteger(0);
	*pState->m_apVariables[g_Config.m_ClDummy] = Stroke;

	if(Stroke != 0 && pState->m_IsMouseAction && g_Config.m_ClSubTickAiming && pState->m_pControls->GameClient()->m_Binds.m_HandlingNewBind)
		pState->m_pControls->m_aMouseOnAction[g_Config.m_ClDummy] = true;
}

void CControls::ConKeyInputCounter(IConsole::IResult *pResult, void *pUserData)
{
	CInputState *pState = (CInputState *)pUserData;

	if((pState->m_pControls->GameClient()->m_GameInfo.m_BugDDRaceInput && pState->m_pControls->GameClient()->m_Snap.m_SpecInfo.m_Active) || pState->m_pControls->GameClient()->m_Spectator.IsActive())
		return;

	int *pVariable = pState->m_apVariables[g_Config.m_ClDummy];
	const int Stroke = pResult->GetInteger(0);
	if(((*pVariable) & 1) != Stroke)
	{
		(*pVariable)++;
		if(Stroke != 0 && pState->m_IsMouseAction && g_Config.m_ClSubTickAiming && pState->m_pControls->GameClient()->m_Binds.m_HandlingNewBind)
			pState->m_pControls->m_aMouseOnAction[g_Config.m_ClDummy] = true;
	}
	*pVariable &= INPUT_STATE_MASK;
}

struct CInputSet
{
	CControls *m_pControls;
	int *m_apVariables[NUM_DUMMIES];
	int m_Value;
};

void CControls::ConKeyInputSet(IConsole::IResult *pResult, void *pUserData)
{
	CInputSet *pSet = (CInputSet *)pUserData;
	if(pResult->GetInteger(0))
	{
		*pSet->m_apVariables[g_Config.m_ClDummy] = pSet->m_Value;
	}
}

void CControls::ConKeyInputNextPrevWeapon(IConsole::IResult *pResult, void *pUserData)
{
	CInputSet *pSet = (CInputSet *)pUserData;
	ConKeyInputCounter(pResult, pSet);
	pSet->m_pControls->m_aInputData[g_Config.m_ClDummy].m_WantedWeapon = 0;
}

void CControls::OnConsoleInit()
{
	// game commands
	{
		static CInputState s_State = {this, {&m_aInputDirectionLeft[0], &m_aInputDirectionLeft[1]}, false};
		Console()->Register("+left", "", CFGFLAG_CLIENT, ConKeyInputState, &s_State, "Move left");
	}
	{
		static CInputState s_State = {this, {&m_aInputDirectionRight[0], &m_aInputDirectionRight[1]}, false};
		Console()->Register("+right", "", CFGFLAG_CLIENT, ConKeyInputState, &s_State, "Move right");
	}
	{
		static CInputState s_State = {this, {&m_aInputData[0].m_Jump, &m_aInputData[1].m_Jump}, false};
		Console()->Register("+jump", "", CFGFLAG_CLIENT, ConKeyInputState, &s_State, "Jump");
	}
	{
		static CInputState s_State = {this, {&m_aInputData[0].m_Hook, &m_aInputData[1].m_Hook}, true};
		Console()->Register("+hook", "", CFGFLAG_CLIENT, ConKeyInputState, &s_State, "Hook");
	}
	{
		static CInputState s_State = {this, {&m_aInputData[0].m_Fire, &m_aInputData[1].m_Fire}, true};
		Console()->Register("+fire", "", CFGFLAG_CLIENT, ConKeyInputCounter, &s_State, "Fire");
	}
	{
		static CInputState s_State = {this, {&m_aShowHookColl[0], &m_aShowHookColl[1]}, false};
		Console()->Register("+showhookcoll", "", CFGFLAG_CLIENT, ConKeyInputState, &s_State, "Show Hook Collision");
	}

	{
		static CInputSet s_Set = {this, {&m_aInputData[0].m_WantedWeapon, &m_aInputData[1].m_WantedWeapon}, 1};
		Console()->Register("+weapon1", "", CFGFLAG_CLIENT, ConKeyInputSet, &s_Set, "Switch to hammer");
	}
	{
		static CInputSet s_Set = {this, {&m_aInputData[0].m_WantedWeapon, &m_aInputData[1].m_WantedWeapon}, 2};
		Console()->Register("+weapon2", "", CFGFLAG_CLIENT, ConKeyInputSet, &s_Set, "Switch to gun");
	}
	{
		static CInputSet s_Set = {this, {&m_aInputData[0].m_WantedWeapon, &m_aInputData[1].m_WantedWeapon}, 3};
		Console()->Register("+weapon3", "", CFGFLAG_CLIENT, ConKeyInputSet, &s_Set, "Switch to shotgun");
	}
	{
		static CInputSet s_Set = {this, {&m_aInputData[0].m_WantedWeapon, &m_aInputData[1].m_WantedWeapon}, 4};
		Console()->Register("+weapon4", "", CFGFLAG_CLIENT, ConKeyInputSet, &s_Set, "Switch to grenade");
	}
	{
		static CInputSet s_Set = {this, {&m_aInputData[0].m_WantedWeapon, &m_aInputData[1].m_WantedWeapon}, 5};
		Console()->Register("+weapon5", "", CFGFLAG_CLIENT, ConKeyInputSet, &s_Set, "Switch to laser");
	}

	{
		static CInputSet s_Set = {this, {&m_aInputData[0].m_NextWeapon, &m_aInputData[1].m_NextWeapon}, 0};
		Console()->Register("+nextweapon", "", CFGFLAG_CLIENT, ConKeyInputNextPrevWeapon, &s_Set, "Switch to next weapon");
	}
	{
		static CInputSet s_Set = {this, {&m_aInputData[0].m_PrevWeapon, &m_aInputData[1].m_PrevWeapon}, 0};
		Console()->Register("+prevweapon", "", CFGFLAG_CLIENT, ConKeyInputNextPrevWeapon, &s_Set, "Switch to previous weapon");
	}

	Console()->Register("tp_to_cursor", "", CFGFLAG_CLIENT, ConTpToCursor, this, "Practice: teleport yourself to your cursor");
	Console()->Register("tp_dummy_to_cursor", "", CFGFLAG_CLIENT, ConTpDummyToCursor, this, "Practice: teleport the other tee to your cursor");
}

void CControls::ConTpToCursor(IConsole::IResult *pResult, void *pUserData)
{
	CControls *pSelf = (CControls *)pUserData;
	if(pSelf->Client()->State() != IClient::STATE_ONLINE)
		return;

	pSelf->SendPracticeTeleportToCursor(g_Config.m_ClDummy ? IClient::CONN_DUMMY : IClient::CONN_MAIN);
}

void CControls::ConTpDummyToCursor(IConsole::IResult *pResult, void *pUserData)
{
	CControls *pSelf = (CControls *)pUserData;
	if(pSelf->Client()->State() != IClient::STATE_ONLINE)
		return;

	if(!pSelf->Client()->DummyConnected())
	{
		pSelf->GameClient()->Echo(Localize("Dummy is not connected"));
		return;
	}

	// Send on the inactive connection; the server only teleports the sender.
	pSelf->SendPracticeTeleportToCursor(g_Config.m_ClDummy ? IClient::CONN_MAIN : IClient::CONN_DUMMY);
}

vec2 CControls::CursorWorldPos() const
{
	const vec2 Center = GameClient()->m_Camera.m_Center;
	const vec2 TargetPos = m_aTargetPos[g_Config.m_ClDummy];
	return Center + (TargetPos - Center) * GameClient()->m_Camera.m_Zoom;
}

void CControls::SendPracticeTeleportToCursor(int Conn)
{
	const vec2 CursorPos = CursorWorldPos();
	CNetMsg_Cl_PracticeTeleport Msg;
	Msg.m_X = round_to_int(CursorPos.x);
	Msg.m_Y = round_to_int(CursorPos.y);
	Client()->SendPackMsg(Conn, &Msg, MSGFLAG_VITAL);
}

void CControls::OnMessage(int Msg, void *pRawMsg)
{
	if(Msg == NETMSGTYPE_SV_WEAPONPICKUP)
	{
		CNetMsg_Sv_WeaponPickup *pMsg = (CNetMsg_Sv_WeaponPickup *)pRawMsg;
		if(g_Config.m_ClAutoswitchWeapons)
			m_aInputData[g_Config.m_ClDummy].m_WantedWeapon = pMsg->m_Weapon + 1;
		// We don't really know ammo count, until we'll switch to that weapon, but any non-zero count will suffice here
		m_aAmmoCount[std::max(0, pMsg->m_Weapon % NUM_WEAPONS)] = 10;
	}
}

int CControls::SnapInput(int *pData)
{
	// живое состояние клавиш прыжка и хука; авойд подменяет их только на время отправки
	const int LiveJump = m_aInputData[g_Config.m_ClDummy].m_Jump;
	const int LiveHook = m_aInputData[g_Config.m_ClDummy].m_Hook;

	// update player state
	if(GameClient()->m_Chat.IsActive())
		m_aInputData[g_Config.m_ClDummy].m_PlayerFlags = PLAYERFLAG_CHATTING;
	else if(GameClient()->m_Menus.IsActive())
		m_aInputData[g_Config.m_ClDummy].m_PlayerFlags = PLAYERFLAG_IN_MENU;
	else
		m_aInputData[g_Config.m_ClDummy].m_PlayerFlags = PLAYERFLAG_PLAYING;

	if(GameClient()->m_Scoreboard.IsActive())
		m_aInputData[g_Config.m_ClDummy].m_PlayerFlags |= PLAYERFLAG_SCOREBOARD;

	if(Client()->ServerCapAnyPlayerFlag() && GameClient()->m_Controls.m_aShowHookColl[g_Config.m_ClDummy])
		m_aInputData[g_Config.m_ClDummy].m_PlayerFlags |= PLAYERFLAG_AIM;

	if(Client()->ServerCapAnyPlayerFlag() && GameClient()->m_Camera.CamType() == CCamera::CAMTYPE_SPEC)
		m_aInputData[g_Config.m_ClDummy].m_PlayerFlags |= PLAYERFLAG_SPEC_CAM;

	switch(m_aMouseInputType[g_Config.m_ClDummy])
	{
	case CControls::EMouseInputType::AUTOMATED:
		m_aInputData[g_Config.m_ClDummy].m_PlayerFlags |= PLAYERFLAG_INPUT_ABSOLUTE;
		break;
	case CControls::EMouseInputType::ABSOLUTE:
		m_aInputData[g_Config.m_ClDummy].m_PlayerFlags |= PLAYERFLAG_INPUT_ABSOLUTE | PLAYERFLAG_INPUT_MANUAL;
		break;
	case CControls::EMouseInputType::RELATIVE:
		m_aInputData[g_Config.m_ClDummy].m_PlayerFlags |= PLAYERFLAG_INPUT_MANUAL;
		break;
	}

	bool Send = m_aLastData[g_Config.m_ClDummy].m_PlayerFlags != m_aInputData[g_Config.m_ClDummy].m_PlayerFlags;

	m_aLastData[g_Config.m_ClDummy].m_PlayerFlags = m_aInputData[g_Config.m_ClDummy].m_PlayerFlags;

	// we freeze the input if chat or menu is activated
	if(!(m_aInputData[g_Config.m_ClDummy].m_PlayerFlags & PLAYERFLAG_PLAYING))
	{
		if(!GameClient()->m_GameInfo.m_BugDDRaceInput)
			ResetInput(g_Config.m_ClDummy);

		mem_copy(pData, &m_aInputData[g_Config.m_ClDummy], sizeof(m_aInputData[0]));

		// set the target anyway though so that we can keep seeing our surroundings,
		// even if chat or menu are activated
		m_aInputData[g_Config.m_ClDummy].m_TargetX = (int)m_aMousePos[g_Config.m_ClDummy].x;
		m_aInputData[g_Config.m_ClDummy].m_TargetY = (int)m_aMousePos[g_Config.m_ClDummy].y;

		// send once a second just to be sure
		Send = Send || time_get() > m_LastSendTime + time_freq();
	}
	else
	{
		m_aInputData[g_Config.m_ClDummy].m_TargetX = (int)m_aMousePos[g_Config.m_ClDummy].x;
		m_aInputData[g_Config.m_ClDummy].m_TargetY = (int)m_aMousePos[g_Config.m_ClDummy].y;

		if(g_Config.m_ClSubTickAiming && m_aMousePosOnAction[g_Config.m_ClDummy] != vec2(0.0f, 0.0f))
		{
			m_aInputData[g_Config.m_ClDummy].m_TargetX = (int)m_aMousePosOnAction[g_Config.m_ClDummy].x;
			m_aInputData[g_Config.m_ClDummy].m_TargetY = (int)m_aMousePosOnAction[g_Config.m_ClDummy].y;
			m_aMousePosOnAction[g_Config.m_ClDummy] = vec2(0.0f, 0.0f);
		}

		if(!m_aInputData[g_Config.m_ClDummy].m_TargetX && !m_aInputData[g_Config.m_ClDummy].m_TargetY)
		{
			m_aInputData[g_Config.m_ClDummy].m_TargetX = 1;
			m_aMousePos[g_Config.m_ClDummy].x = 1;
		}

		// set direction
		m_aInputData[g_Config.m_ClDummy].m_Direction = 0;
		if(m_aInputDirectionLeft[g_Config.m_ClDummy] && !m_aInputDirectionRight[g_Config.m_ClDummy])
			m_aInputData[g_Config.m_ClDummy].m_Direction = -1;
		if(!m_aInputDirectionLeft[g_Config.m_ClDummy] && m_aInputDirectionRight[g_Config.m_ClDummy])
			m_aInputData[g_Config.m_ClDummy].m_Direction = 1;

		// dummy copy moves
		if(g_Config.m_ClDummyCopyMoves)
		{
			CNetObj_PlayerInput *pDummyInput = &GameClient()->m_DummyInput;

			// Don't copy any input to dummy when spectating others
			if(!GameClient()->m_Snap.m_SpecInfo.m_Active || GameClient()->m_Snap.m_SpecInfo.m_SpectatorId < 0)
			{
				pDummyInput->m_Direction = m_aInputData[g_Config.m_ClDummy].m_Direction;
				pDummyInput->m_Hook = m_aInputData[g_Config.m_ClDummy].m_Hook;
				pDummyInput->m_Jump = m_aInputData[g_Config.m_ClDummy].m_Jump;
				pDummyInput->m_PlayerFlags = m_aInputData[g_Config.m_ClDummy].m_PlayerFlags;
				pDummyInput->m_TargetX = m_aInputData[g_Config.m_ClDummy].m_TargetX;
				pDummyInput->m_TargetY = m_aInputData[g_Config.m_ClDummy].m_TargetY;
				pDummyInput->m_WantedWeapon = m_aInputData[g_Config.m_ClDummy].m_WantedWeapon;

				if(!g_Config.m_ClDummyControl)
					pDummyInput->m_Fire += m_aInputData[g_Config.m_ClDummy].m_Fire - m_aLastData[g_Config.m_ClDummy].m_Fire;

				pDummyInput->m_NextWeapon += m_aInputData[g_Config.m_ClDummy].m_NextWeapon - m_aLastData[g_Config.m_ClDummy].m_NextWeapon;
				pDummyInput->m_PrevWeapon += m_aInputData[g_Config.m_ClDummy].m_PrevWeapon - m_aLastData[g_Config.m_ClDummy].m_PrevWeapon;
			}

			m_aInputData[!g_Config.m_ClDummy] = *pDummyInput;
		}

		if(g_Config.m_ClDummyControl)
		{
			CNetObj_PlayerInput *pDummyInput = &GameClient()->m_DummyInput;
			pDummyInput->m_Jump = g_Config.m_ClDummyJump;

			if(g_Config.m_ClDummyFire)
				pDummyInput->m_Fire = g_Config.m_ClDummyFire;
			else if((pDummyInput->m_Fire & 1) != 0)
				pDummyInput->m_Fire++;

			pDummyInput->m_Hook = g_Config.m_ClDummyHook;
			m_aInputData[!g_Config.m_ClDummy] = *pDummyInput;
		}

		// stress testing
		if(g_Config.m_DbgStress)
		{
			float t = Client()->LocalTime();
			mem_zero(&m_aInputData[g_Config.m_ClDummy], sizeof(m_aInputData[0]));

			m_aInputData[g_Config.m_ClDummy].m_Direction = ((int)t / 2) & 1;
			m_aInputData[g_Config.m_ClDummy].m_Jump = ((int)t);
			m_aInputData[g_Config.m_ClDummy].m_Fire = ((int)(t * 10));
			m_aInputData[g_Config.m_ClDummy].m_Hook = ((int)(t * 2)) & 1;
			m_aInputData[g_Config.m_ClDummy].m_WantedWeapon = ((int)t) % NUM_WEAPONS;
			m_aInputData[g_Config.m_ClDummy].m_TargetX = (int)(std::sin(t * 3) * 100.0f);
			m_aInputData[g_Config.m_ClDummy].m_TargetY = (int)(std::cos(t * 3) * 100.0f);
		}

		if(g_Config.m_ClAvoid)
			ApplyAvoid(&m_aInputData[g_Config.m_ClDummy]);

		// check if we need to send input
		Send = Send || m_aInputData[g_Config.m_ClDummy].m_Direction != m_aLastData[g_Config.m_ClDummy].m_Direction;
		Send = Send || m_aInputData[g_Config.m_ClDummy].m_Jump != m_aLastData[g_Config.m_ClDummy].m_Jump;
		Send = Send || m_aInputData[g_Config.m_ClDummy].m_Fire != m_aLastData[g_Config.m_ClDummy].m_Fire;
		Send = Send || m_aInputData[g_Config.m_ClDummy].m_Hook != m_aLastData[g_Config.m_ClDummy].m_Hook;
		Send = Send || m_aInputData[g_Config.m_ClDummy].m_WantedWeapon != m_aLastData[g_Config.m_ClDummy].m_WantedWeapon;
		Send = Send || m_aInputData[g_Config.m_ClDummy].m_NextWeapon != m_aLastData[g_Config.m_ClDummy].m_NextWeapon;
		Send = Send || m_aInputData[g_Config.m_ClDummy].m_PrevWeapon != m_aLastData[g_Config.m_ClDummy].m_PrevWeapon;
		Send = Send || time_get() > m_LastSendTime + time_freq() / 25; // send at least 25 Hz
		Send = Send || (GameClient()->m_Snap.m_pLocalCharacter && GameClient()->m_Snap.m_pLocalCharacter->m_Weapon == WEAPON_NINJA && (m_aInputData[g_Config.m_ClDummy].m_Direction || m_aInputData[g_Config.m_ClDummy].m_Jump || m_aInputData[g_Config.m_ClDummy].m_Hook));
	}

	// copy and return size
	m_aLastData[g_Config.m_ClDummy] = m_aInputData[g_Config.m_ClDummy];

	if(!Send)
	{
		m_aInputData[g_Config.m_ClDummy].m_Jump = LiveJump;
		m_aInputData[g_Config.m_ClDummy].m_Hook = LiveHook;
		return 0;
	}

	m_LastSendTime = time_get();
	mem_copy(pData, &m_aInputData[g_Config.m_ClDummy], sizeof(m_aInputData[0]));
	m_aInputData[g_Config.m_ClDummy].m_Jump = LiveJump;
	m_aInputData[g_Config.m_ClDummy].m_Hook = LiveHook;
	return sizeof(m_aInputData[0]);
}
void CControls::ApplyAvoid(CNetObj_PlayerInput *pInput)
{
	const int Dummy = g_Config.m_ClDummy;
	const int LocalId = GameClient()->m_aLocalIds[Dummy];
	const CGameClient::CClientData *pData = nullptr;
	if(LocalId >= 0 && LocalId < MAX_CLIENTS && GameClient()->m_Snap.m_aCharacters[LocalId].m_Active && !GameClient()->m_Snap.m_SpecInfo.m_Active)
		pData = &GameClient()->m_aClients[LocalId];
	if(pData == nullptr || !pData->m_IsPredicted)
	{
		m_aAvoidHolding[Dummy] = false;
		return;
	}
	const CCharacterCore &Core = pData->m_Predicted;
	if(Core.m_FreezeEnd != 0 || Core.m_DeepFrozen || Core.m_Super)
	{
		m_aAvoidHolding[Dummy] = false;
		return;
	}

	const CCollision *pCollision = Collision();
	const SAvoidTune Tune = ReadTune(Core.m_Tuning);
	const vec2 Pos = Core.m_Pos;
	const vec2 Vel = Core.m_Vel;
	const int Tick = Client()->PredGameTick(Dummy);
	const float ReactDist = (float)g_Config.m_ClAvoidRange * AVOID_TILE_SIZE;
	const int UserDir = pInput->m_Direction;
	const int LiveJump = pInput->m_Jump;
	const int LiveHook = pInput->m_Hook;
	const bool OnGround = pCollision->IsOnGround(Pos, CCharacterCore::PhysicalSize());
	const bool CanJump = OnGround || (Core.m_Jumped & 2) == 0;
	const bool HookBusy = Core.m_HookState == HOOK_GRABBED || Core.m_HookState == HOOK_FLYING;
	const bool HookAllowed = g_Config.m_ClAvoidHook != 0 && Tick >= m_aAvoidBlockUntil[Dummy];
	const bool UserHookPulling = g_Config.m_ClAvoidFilter != 0 && LiveHook != 0 && Core.m_HookState == HOOK_GRABBED && !m_aAvoidHolding[Dummy];

	SAvoidPlan Plan;
	bool Threat;
	if(m_aAvoidHolding[Dummy] && HookAllowed && HookBusy)
	{
		// Держим свой хук, пока без него тия всё ещё попадала бы в опасность рядом.
		Threat = SimDangerTick(pCollision, Tune, Pos, Vel, UserDir, AVOID_LOOKAHEAD_TICKS, ReactDist) >= 0;
		Plan.m_Direction = UserDir;
		Plan.m_Hook = Threat;
	}
	else
	{
		Threat = PlanAvoid(pCollision, Tune, Pos, Vel, UserDir, CanJump, g_Config.m_ClAvoidWalk != 0, g_Config.m_ClAvoidJump != 0, HookAllowed, ReactDist, UserHookPulling, Core.m_HookPos, &Plan);
	}

	bool AvoidJumps = false;
	if(Threat)
	{
		pInput->m_Direction = Plan.m_Direction;
		if(Plan.m_Hook)
		{
			if(!m_aAvoidHolding[Dummy])
			{
				m_aAvoidHolding[Dummy] = true;
				m_aAvoidHoldStart[Dummy] = Tick;
			}
			if(Tick - m_aAvoidHoldStart[Dummy] > AVOID_MAX_HOLD_SECONDS * Client()->GameTickSpeed())
			{
				m_aAvoidHolding[Dummy] = false;
				m_aAvoidBlockUntil[Dummy] = Tick + 10;
			}
			else
			{
				if(!HookBusy)
				{
					pInput->m_TargetX = round_to_int(Plan.m_HookTarget.x - Pos.x);
					pInput->m_TargetY = round_to_int(Plan.m_HookTarget.y - Pos.y);
				}
				pInput->m_Hook = 1;
			}
		}
		else
		{
			m_aAvoidHolding[Dummy] = false;
			if(Plan.m_Release)
			{
				pInput->m_Hook = 0;
				m_aAvoidReleaseUntil[Dummy] = Tick + 10;
			}
			if(Plan.m_Jump)
			{
				pInput->m_Jump = m_aLastData[Dummy].m_Jump == 0 ? 1 : 0;
				AvoidJumps = true;
			}
		}
	}
	else
	{
		m_aAvoidHolding[Dummy] = false;
	}

	// Сразу после отпускания не даём твоему зажатому хуку тут же зацепиться снова.
	if(Tick < m_aAvoidReleaseUntil[Dummy] && !(Threat && Plan.m_Hook))
		pInput->m_Hook = 0;

	// Прыжок блокируется только если он ведёт во фриз в пределах радиуса реакции.
	if(g_Config.m_ClAvoidFilter && !AvoidJumps && LiveJump && CanJump)
	{
		const float Impulse = OnGround ? Tune.m_GroundJump : Tune.m_AirJump;
		if(SimJumpDangerTick(pCollision, Tune, Pos, Vel, Impulse, pInput->m_Direction, AVOID_LOOKAHEAD_TICKS, ReactDist) >= 0)
			pInput->m_Jump = 0;
	}
}

void CControls::OnRender()
{
	if(Client()->State() != IClient::STATE_ONLINE && Client()->State() != IClient::STATE_DEMOPLAYBACK)
		return;

	if(g_Config.m_ClAutoswitchWeaponsOutOfAmmo && !GameClient()->m_GameInfo.m_UnlimitedAmmo && GameClient()->m_Snap.m_pLocalCharacter)
	{
		// Keep track of ammo count, we know weapon ammo only when we switch to that weapon, this is tracked on server and protocol does not track that
		m_aAmmoCount[std::max(0, GameClient()->m_Snap.m_pLocalCharacter->m_Weapon % NUM_WEAPONS)] = GameClient()->m_Snap.m_pLocalCharacter->m_AmmoCount;
		// Autoswitch weapon if we're out of ammo
		if(m_aInputData[g_Config.m_ClDummy].m_Fire % 2 != 0 &&
			GameClient()->m_Snap.m_pLocalCharacter->m_AmmoCount == 0 &&
			GameClient()->m_Snap.m_pLocalCharacter->m_Weapon != WEAPON_HAMMER &&
			GameClient()->m_Snap.m_pLocalCharacter->m_Weapon != WEAPON_NINJA)
		{
			int Weapon;
			for(Weapon = WEAPON_LASER; Weapon > WEAPON_GUN; Weapon--)
			{
				if(Weapon == GameClient()->m_Snap.m_pLocalCharacter->m_Weapon)
					continue;
				if(m_aAmmoCount[Weapon] > 0)
					break;
			}
			if(Weapon != GameClient()->m_Snap.m_pLocalCharacter->m_Weapon)
				m_aInputData[g_Config.m_ClDummy].m_WantedWeapon = Weapon + 1;
		}
	}

	// update target pos
	if(GameClient()->m_Snap.m_pGameInfoObj && !GameClient()->m_Snap.m_SpecInfo.m_Active)
	{
		// make sure to compensate for smooth dyncam to ensure the cursor stays still in world space if zoomed
		vec2 DyncamOffsetDelta = GameClient()->m_Camera.m_DyncamTargetCameraOffset - GameClient()->m_Camera.m_aDyncamCurrentCameraOffset[g_Config.m_ClDummy];
		float Zoom = GameClient()->m_Camera.m_Zoom;
		m_aTargetPos[g_Config.m_ClDummy] = GameClient()->m_LocalCharacterPos + m_aMousePos[g_Config.m_ClDummy] - DyncamOffsetDelta + DyncamOffsetDelta / Zoom;
	}
	else if(GameClient()->m_Snap.m_SpecInfo.m_Active && GameClient()->m_Snap.m_SpecInfo.m_UsePosition)
	{
		m_aTargetPos[g_Config.m_ClDummy] = GameClient()->m_Snap.m_SpecInfo.m_Position + m_aMousePos[g_Config.m_ClDummy];
	}
	else
	{
		m_aTargetPos[g_Config.m_ClDummy] = m_aMousePos[g_Config.m_ClDummy];
	}
}

bool CControls::OnCursorMove(float x, float y, IInput::ECursorType CursorType)
{
	if(GameClient()->IsWorldPaused())
		return false;

	if(CursorType == IInput::CURSOR_JOYSTICK && g_Config.m_InpControllerAbsolute && GameClient()->m_Snap.m_pGameInfoObj && !GameClient()->m_Snap.m_SpecInfo.m_Active)
	{
		vec2 AbsoluteDirection;
		if(Input()->GetActiveJoystick()->Absolute(&AbsoluteDirection.x, &AbsoluteDirection.y))
		{
			m_aMousePos[g_Config.m_ClDummy] = AbsoluteDirection * GetMaxMouseDistance();
			GameClient()->m_Controls.m_aMouseInputType[g_Config.m_ClDummy] = CControls::EMouseInputType::ABSOLUTE;
		}
		return true;
	}

	float Factor = 1.0f;
	if(g_Config.m_ClDyncam && g_Config.m_ClDyncamMousesens)
	{
		Factor = g_Config.m_ClDyncamMousesens / 100.0f;
	}
	else
	{
		switch(CursorType)
		{
		case IInput::CURSOR_MOUSE:
			Factor = g_Config.m_InpMousesens / 100.0f;
			break;
		case IInput::CURSOR_JOYSTICK:
			Factor = g_Config.m_InpControllerSens / 100.0f;
			break;
		default:
			dbg_assert_failed("CControls::OnCursorMove CursorType %d", (int)CursorType);
		}
	}

	if(GameClient()->m_Snap.m_SpecInfo.m_Active && GameClient()->m_Snap.m_SpecInfo.m_SpectatorId < 0)
		Factor *= GameClient()->m_Camera.m_Zoom;

	m_aMousePos[g_Config.m_ClDummy] += vec2(x, y) * Factor;
	GameClient()->m_Controls.m_aMouseInputType[g_Config.m_ClDummy] = CControls::EMouseInputType::RELATIVE;
	ClampMousePos();
	return true;
}

void CControls::ClampMousePos()
{
	if(GameClient()->m_Snap.m_SpecInfo.m_Active && GameClient()->m_Snap.m_SpecInfo.m_SpectatorId < 0)
	{
		m_aMousePos[g_Config.m_ClDummy].x = std::clamp(m_aMousePos[g_Config.m_ClDummy].x, -201.0f * 32, (Collision()->GetWidth() + 201.0f) * 32.0f);
		m_aMousePos[g_Config.m_ClDummy].y = std::clamp(m_aMousePos[g_Config.m_ClDummy].y, -201.0f * 32, (Collision()->GetHeight() + 201.0f) * 32.0f);
	}
	else
	{
		const float MouseMin = GetMinMouseDistance();
		const float MouseMax = GetMaxMouseDistance();

		float MouseDistance = length(m_aMousePos[g_Config.m_ClDummy]);
		if(MouseDistance < 0.001f)
		{
			m_aMousePos[g_Config.m_ClDummy].x = 0.001f;
			m_aMousePos[g_Config.m_ClDummy].y = 0;
			MouseDistance = 0.001f;
		}
		if(MouseDistance < MouseMin)
			m_aMousePos[g_Config.m_ClDummy] = normalize_pre_length(m_aMousePos[g_Config.m_ClDummy], MouseDistance) * MouseMin;
		MouseDistance = length(m_aMousePos[g_Config.m_ClDummy]);
		if(MouseDistance > MouseMax)
			m_aMousePos[g_Config.m_ClDummy] = normalize_pre_length(m_aMousePos[g_Config.m_ClDummy], MouseDistance) * MouseMax;
	}
}

float CControls::GetMinMouseDistance() const
{
	return g_Config.m_ClDyncam ? g_Config.m_ClDyncamMinDistance : g_Config.m_ClMouseMinDistance;
}

float CControls::GetMaxMouseDistance() const
{
	float CameraMaxDistance = 200.0f;
	float FollowFactor = (g_Config.m_ClDyncam ? g_Config.m_ClDyncamFollowFactor : g_Config.m_ClMouseFollowfactor) / 100.0f;
	float DeadZone = g_Config.m_ClDyncam ? g_Config.m_ClDyncamDeadzone : g_Config.m_ClMouseDeadzone;
	float MaxDistance = g_Config.m_ClDyncam ? g_Config.m_ClDyncamMaxDistance : g_Config.m_ClMouseMaxDistance;
	return std::min(FollowFactor != 0.0f ? CameraMaxDistance / FollowFactor + DeadZone : MaxDistance, MaxDistance);
}
