/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include "menus.h"

#include <base/dbg.h>
#include <base/math.h>
#include <base/str.h>

#include <engine/graphics.h>
#include <engine/shared/config.h>

#include <game/client/components/menu_background.h>
#include <game/client/gameclient.h>
#include <game/client/ui.h>
#include <game/localization.h>

#include <algorithm>
#include <cstdio>

namespace {

// Название клиента во вкладке настроек. Поменяй на своё.
constexpr const char *CLIENT_MENU_NAME = "Zhonal";
constexpr int CLIENT_PRESET_SLOTS = 3;

struct SClientSetting
{
	int *m_pValue;
	int m_Min;
	int m_Max;
	int m_Default;
};

// Порядок важен: по нему настройки пишутся в пресет. Новые настройки добавляй ТОЛЬКО в конец списка.
const SClientSetting *ClientSettings(int *pNum)
{
	static const SClientSetting s_aSettings[] = {
		{&g_Config.m_ClAvoid, 0, 1, 0},
		{&g_Config.m_ClAvoidWalk, 0, 1, 1},
		{&g_Config.m_ClAvoidJump, 0, 1, 1},
		{&g_Config.m_ClAvoidHook, 0, 1, 1},
		{&g_Config.m_ClAvoidFilter, 0, 1, 1},
		{&g_Config.m_ClAvoidHoldMax, 1, 10, 3},
		{&g_Config.m_ClKagune, 0, 1, 1},
		{&g_Config.m_ClKaguneR, 0, 255, 200},
		{&g_Config.m_ClKaguneG, 0, 255, 10},
		{&g_Config.m_ClKaguneB, 0, 255, 25},
		{&g_Config.m_ClKaguneWidth, 1, 20, 6},
		{&g_Config.m_ClKaguneCount, 1, 8, 5},
		{&g_Config.m_ClKaguneWave, 0, 50, 14},
		{&g_Config.m_ClKaguneLen, 30, 300, 100},
		{&g_Config.m_ClKaguneStyle, 0, 3, 0},
		{&g_Config.m_ClKaguneShimmer, 0, 100, 60},
		{&g_Config.m_ClHammerFx, 0, 1, 1},
		{&g_Config.m_ClHammerFxR, 0, 255, 255},
		{&g_Config.m_ClHammerFxG, 0, 255, 120},
		{&g_Config.m_ClHammerFxB, 0, 255, 20},
		{&g_Config.m_ClHammerFxSize, 25, 300, 100},
	};
	*pNum = (int)(sizeof(s_aSettings) / sizeof(s_aSettings[0]));
	return s_aSettings;
}

void SaveClientPreset(char *pDest, int DestSize)
{
	int Num = 0;
	const SClientSetting *pSettings = ClientSettings(&Num);
	char aBuf[256];
	int Len = 0;
	aBuf[0] = '\0';
	for(int i = 0; i < Num && Len < (int)sizeof(aBuf) - 8; i++)
		Len += std::snprintf(aBuf + Len, sizeof(aBuf) - Len, "%d ", *pSettings[i].m_pValue);
	str_copy(pDest, aBuf, DestSize);
}

// Загружает столько значений, сколько есть в строке; всё за пределами диапазона обрезается.
void LoadClientPreset(const char *pSrc)
{
	int Num = 0;
	const SClientSetting *pSettings = ClientSettings(&Num);
	for(int i = 0; i < Num; i++)
	{
		int Value = 0;
		int Used = 0;
		if(std::sscanf(pSrc, "%d%n", &Value, &Used) != 1)
			break;
		pSrc += Used;
		*pSettings[i].m_pValue = std::clamp(Value, pSettings[i].m_Min, pSettings[i].m_Max);
	}
}

void ResetClientSettings()
{
	int Num = 0;
	const SClientSetting *pSettings = ClientSettings(&Num);
	for(int i = 0; i < Num; i++)
		*pSettings[i].m_pValue = pSettings[i].m_Default;
}

} // namespace

void CMenus::SetNeedSendInfo()
{
	if(m_Dummy)
		m_NeedSendDummyinfo = true;
	else
		m_NeedSendinfo = true;
}

void CMenus::RenderSettings(CUIRect MainView)
{
	// render background
	CUIRect Button, TabBar, RestartBar;
	MainView.VSplitRight(120.0f, &MainView, &TabBar);
	MainView.Draw(ms_ColorTabbarActive, IGraphics::CORNER_B, 10.0f);
	MainView.Margin(20.0f, &MainView);

	const bool NeedRestart = m_NeedRestartGraphics || m_NeedRestartSound || m_NeedRestartUpdate;
	if(NeedRestart)
	{
		MainView.HSplitBottom(20.0f, &MainView, &RestartBar);
		MainView.HSplitBottom(10.0f, &MainView, nullptr);
	}

	TabBar.HSplitTop(50.0f, &Button, &TabBar);
	Button.Draw(ms_ColorTabbarActive, IGraphics::CORNER_BR, 10.0f);

	const char *apTabs[SETTINGS_LENGTH] = {
		Localize("Language"),
		Localize("General"),
		Localize("Player"),
		Client()->IsSixup() ? "Tee 0.7" : Localize("Tee"),
		Localize("Appearance"),
		Localize("Controls"),
		Localize("Graphics"),
		Localize("Sound"),
		Localize("DDNet"),
		Localize("Assets"),
		Localize("Credits"),
		CLIENT_MENU_NAME};
	static CButtonContainer s_aTabButtons[SETTINGS_LENGTH];

	for(int i = 0; i < SETTINGS_LENGTH; i++)
	{
		TabBar.HSplitTop(10.0f, nullptr, &TabBar);
		TabBar.HSplitTop(26.0f, &Button, &TabBar);
		if(DoButton_MenuTab(&s_aTabButtons[i], apTabs[i], g_Config.m_UiSettingsPage == i, &Button, IGraphics::CORNER_R, &m_aAnimatorsSettingsTab[i]))
			g_Config.m_UiSettingsPage = i;
	}

	if(g_Config.m_UiSettingsPage == SETTINGS_LANGUAGE)
	{
		GameClient()->m_MenuBackground.ChangePosition(CMenuBackground::POS_SETTINGS_LANGUAGE);
		RenderLanguageSettings(MainView);
	}
	else if(g_Config.m_UiSettingsPage == SETTINGS_GENERAL)
	{
		GameClient()->m_MenuBackground.ChangePosition(CMenuBackground::POS_SETTINGS_GENERAL);
		RenderSettingsGeneral(MainView);
	}
	else if(g_Config.m_UiSettingsPage == SETTINGS_PLAYER)
	{
		GameClient()->m_MenuBackground.ChangePosition(CMenuBackground::POS_SETTINGS_PLAYER);
		RenderSettingsPlayer(MainView);
	}
	else if(g_Config.m_UiSettingsPage == SETTINGS_TEE)
	{
		GameClient()->m_MenuBackground.ChangePosition(CMenuBackground::POS_SETTINGS_TEE);
		if(Client()->IsSixup())
			RenderSettingsTee7(MainView);
		else
			RenderSettingsTee(MainView);
	}
	else if(g_Config.m_UiSettingsPage == SETTINGS_APPEARANCE)
	{
		GameClient()->m_MenuBackground.ChangePosition(CMenuBackground::POS_SETTINGS_APPEARANCE);
		RenderSettingsAppearance(MainView);
	}
	else if(g_Config.m_UiSettingsPage == SETTINGS_CONTROLS)
	{
		GameClient()->m_MenuBackground.ChangePosition(CMenuBackground::POS_SETTINGS_CONTROLS);
		m_MenusSettingsControls.Render(MainView);
	}
	else if(g_Config.m_UiSettingsPage == SETTINGS_GRAPHICS)
	{
		GameClient()->m_MenuBackground.ChangePosition(CMenuBackground::POS_SETTINGS_GRAPHICS);
		RenderSettingsGraphics(MainView);
	}
	else if(g_Config.m_UiSettingsPage == SETTINGS_SOUND)
	{
		GameClient()->m_MenuBackground.ChangePosition(CMenuBackground::POS_SETTINGS_SOUND);
		RenderSettingsSound(MainView);
	}
	else if(g_Config.m_UiSettingsPage == SETTINGS_DDNET)
	{
		GameClient()->m_MenuBackground.ChangePosition(CMenuBackground::POS_SETTINGS_DDNET);
		RenderSettingsDDNet(MainView);
	}
	else if(g_Config.m_UiSettingsPage == SETTINGS_ASSETS)
	{
		GameClient()->m_MenuBackground.ChangePosition(CMenuBackground::POS_SETTINGS_ASSETS);
		RenderSettingsAssets(MainView);
	}
	else if(g_Config.m_UiSettingsPage == SETTINGS_CREDITS)
	{
		GameClient()->m_MenuBackground.ChangePosition(CMenuBackground::POS_SETTINGS_CREDITS);
		RenderSettingsCredits(MainView);
	}
			else if(g_Config.m_UiSettingsPage == SETTINGS_CLIENT)
	{
		GameClient()->m_MenuBackground.ChangePosition(CMenuBackground::POS_SETTINGS_DDNET);
		RenderSettingsClient(MainView);
	}
	else
	{
		dbg_assert_failed("ui_settings_page invalid");
	}

	if(NeedRestart)
	{
		CUIRect RestartWarning, RestartButton;
		RestartBar.VSplitRight(125.0f, &RestartWarning, &RestartButton);
		RestartWarning.VSplitRight(10.0f, &RestartWarning, nullptr);
		if(m_NeedRestartUpdate)
		{
			TextRender()->TextColor(1.0f, 0.4f, 0.4f, 1.0f);
			Ui()->DoLabel(&RestartWarning, Localize("DDNet Client needs to be restarted to complete update!"), 14.0f, TEXTALIGN_ML);
			TextRender()->TextColor(1.0f, 1.0f, 1.0f, 1.0f);
		}
		else
		{
			Ui()->DoLabel(&RestartWarning, Localize("You must restart the game for all settings to take effect."), 14.0f, TEXTALIGN_ML);
		}

		static CButtonContainer s_RestartButton;
		if(DoButton_Menu(&s_RestartButton, Localize("Restart"), 0, &RestartButton))
		{
			if(Client()->State() == IClient::STATE_ONLINE || GameClient()->Editor()->HasUnsavedData())
			{
				m_Popup = POPUP_RESTART;
			}
			else
			{
				Client()->Restart();
			}
		}
	}
}

void CMenus::RenderSettingsClient(CUIRect MainView)
{
	static int s_Page = 0;
	static CButtonContainer s_aPageButtons[4];
	static CButtonContainer s_aSaveButtons[CLIENT_PRESET_SLOTS];
	static CButtonContainer s_aLoadButtons[CLIENT_PRESET_SLOTS];
	static CButtonContainer s_aClearButtons[CLIENT_PRESET_SLOTS];
	static CButtonContainer s_ResetButton;

	const float LineHeight = 20.0f;
	const float RowGap = 5.0f;

	CUIRect Title, PageBar;
	MainView.HSplitTop(24.0f, &Title, &MainView);
	Ui()->DoLabel(&Title, CLIENT_MENU_NAME, 20.0f, TEXTALIGN_ML);
	MainView.HSplitTop(RowGap, nullptr, &MainView);

	MainView.HSplitTop(25.0f, &PageBar, &MainView);
	MainView.HSplitTop(RowGap * 2.0f, nullptr, &MainView);
	const char *apPages[4] = {"Авойд", "Кагуне", "Молот", "Конфиги"};
	const float PageWidth = PageBar.w / 4.0f;
	for(int i = 0; i < 4; i++)
	{
		CUIRect Button;
		PageBar.VSplitLeft(PageWidth, &Button, &PageBar);
		if(DoButton_Menu(&s_aPageButtons[i], apPages[i], s_Page == i, &Button))
			s_Page = i;
	}

	auto NextRow = [&](CUIRect *pRow) {
		MainView.HSplitTop(LineHeight, pRow, &MainView);
		MainView.HSplitTop(RowGap, nullptr, &MainView);
	};
	auto Toggle = [&](int *pValue, const char *pText) {
		CUIRect Row;
		NextRow(&Row);
		if(DoButton_CheckBox(pValue, pText, *pValue, &Row))
			*pValue = *pValue ? 0 : 1;
	};
	auto Slider = [&](int *pValue, const char *pText, int Min, int Max) {
		CUIRect Row, Label;
		NextRow(&Row);
		Row.VSplitLeft(260.0f, &Label, &Row);
		char aBuf[96];
		str_format(aBuf, sizeof(aBuf), "%s: %d", pText, *pValue);
		Ui()->DoLabel(&Label, aBuf, 14.0f, TEXTALIGN_ML);
		Row.Draw(ColorRGBA(0.15f, 0.15f, 0.15f, 1.0f), IGraphics::CORNER_ALL, 1.0f);
		ColorRGBA HandleColor(1.0f, 1.0f, 1.0f, 1.0f);
		const float Norm = Max > Min ? (float)(*pValue - Min) / (float)(Max - Min) : 0.0f;
		const float NewNorm = Ui()->DoScrollbarH(pValue, &Row, Norm, &HandleColor);
		*pValue = Min + round_to_int(NewNorm * (float)(Max - Min));
	};

	if(s_Page == 0)
	{
		Toggle(&g_Config.m_ClAvoid, "Авойд (главный выключатель)");
		Toggle(&g_Config.m_ClAvoidWalk, "Уходить от опасности ходьбой");
		Toggle(&g_Config.m_ClAvoidJump, "Прыгать от опасности");
		Toggle(&g_Config.m_ClAvoidHook, "Хукать от опасности");
		Toggle(&g_Config.m_ClAvoidFilter, "Отменять мой хук и прыжок, если ведут в опасность");
		Slider(&g_Config.m_ClAvoidHoldMax, "Макс. удержание хука (сек)", 1, 10);
	}
	else if(s_Page == 1)
	{
		Toggle(&g_Config.m_ClKagune, "Кагуне");
		Slider(&g_Config.m_ClKaguneStyle, "Стиль (0 веер, 1 осьминог, 2 крылья, 3 хлысты)", 0, 3);
		Slider(&g_Config.m_ClKaguneCount, "Количество", 1, 8);
		Slider(&g_Config.m_ClKaguneWidth, "Толщина", 1, 20);
		Slider(&g_Config.m_ClKaguneWave, "Волна", 0, 50);
		Slider(&g_Config.m_ClKaguneLen, "Длина", 30, 300);
		Slider(&g_Config.m_ClKaguneShimmer, "Блеск", 0, 100);
		Slider(&g_Config.m_ClKaguneR, "Красный", 0, 255);
		Slider(&g_Config.m_ClKaguneG, "Зелёный", 0, 255);
		Slider(&g_Config.m_ClKaguneB, "Синий", 0, 255);
	}
	else if(s_Page == 2)
	{
		Toggle(&g_Config.m_ClHammerFx, "Эффект удара молотом");
		Slider(&g_Config.m_ClHammerFxR, "Красный", 0, 255);
		Slider(&g_Config.m_ClHammerFxG, "Зелёный", 0, 255);
		Slider(&g_Config.m_ClHammerFxB, "Синий", 0, 255);
		Slider(&g_Config.m_ClHammerFxSize, "Размер (%)", 25, 300);
	}
	else
	{
		char *apSlots[CLIENT_PRESET_SLOTS] = {g_Config.m_ClClientPreset1, g_Config.m_ClClientPreset2, g_Config.m_ClClientPreset3};
		for(int i = 0; i < CLIENT_PRESET_SLOTS; i++)
		{
			CUIRect Row, Label, Button;
			NextRow(&Row);
			Row.VSplitLeft(140.0f, &Label, &Row);
			char aBuf[64];
			str_format(aBuf, sizeof(aBuf), "Слот %d%s", i + 1, apSlots[i][0] == '\0' ? " (пусто)" : "");
			Ui()->DoLabel(&Label, aBuf, 14.0f, TEXTALIGN_ML);
			Row.VSplitLeft(110.0f, &Button, &Row);
			if(DoButton_Menu(&s_aSaveButtons[i], "Сохранить", 0, &Button))
				SaveClientPreset(apSlots[i], sizeof(g_Config.m_ClClientPreset1));
			Row.VSplitLeft(5.0f, nullptr, &Row);
			Row.VSplitLeft(110.0f, &Button, &Row);
			if(DoButton_Menu(&s_aLoadButtons[i], "Загрузить", 0, &Button))
				LoadClientPreset(apSlots[i]);
			Row.VSplitLeft(5.0f, nullptr, &Row);
			Row.VSplitLeft(110.0f, &Button, &Row);
			if(DoButton_Menu(&s_aClearButtons[i], "Очистить", 0, &Button))
				apSlots[i][0] = '\0';
		}
		CUIRect ResetRow, ResetButton;
		MainView.HSplitTop(RowGap, nullptr, &MainView);
		NextRow(&ResetRow);
		ResetRow.VSplitLeft(260.0f, &ResetButton, nullptr);
		if(DoButton_Menu(&s_ResetButton, "Сбросить всё по умолчанию", 0, &ResetButton))
			ResetClientSettings();
	}
}

bool CMenus::RenderHslaScrollbars(CUIRect *pRect, unsigned int *pColor, bool Alpha, float DarkestLight)
{
	const unsigned PrevPackedColor = *pColor;
	ColorHSLA Color(*pColor, Alpha);
	const ColorHSLA OriginalColor = Color;
	const char *apLabels[] = {Localize("Hue"), Localize("Sat."), Localize("Lht."), Localize("Alpha")};
	const float SizePerEntry = 20.0f;
	const float MarginPerEntry = 5.0f;
	const float PreviewMargin = 2.5f;
	const float PreviewHeight = 40.0f + 2 * PreviewMargin;
	const float OffY = (SizePerEntry + MarginPerEntry) * (3 + (Alpha ? 1 : 0)) - PreviewHeight;

	CUIRect Preview;
	pRect->VSplitLeft(PreviewHeight, &Preview, pRect);
	Preview.HSplitTop(OffY / 2.0f, nullptr, &Preview);
	Preview.HSplitTop(PreviewHeight, &Preview, nullptr);

	Preview.Draw(ColorRGBA(0.15f, 0.15f, 0.15f, 1.0f), IGraphics::CORNER_ALL, 4.0f + PreviewMargin);
	Preview.Margin(PreviewMargin, &Preview);
	Preview.Draw(color_cast<ColorRGBA>(Color.UnclampLighting(DarkestLight)), IGraphics::CORNER_ALL, 4.0f + PreviewMargin);

	auto &&RenderHueRect = [&](CUIRect *pColorRect) {
		float CurXOff = pColorRect->x;
		const float SizeColor = pColorRect->w / 6;

		// red to yellow
		{
			Graphics()->SetColor4(
				ColorRGBA(1, 0, 0, 1),
				ColorRGBA(1, 1, 0, 1),
				ColorRGBA(1, 1, 0, 1),
				ColorRGBA(1, 0, 0, 1));

			IGraphics::CFreeformItem Freeform(
				CurXOff, pColorRect->y,
				CurXOff + SizeColor, pColorRect->y,
				CurXOff, pColorRect->y + pColorRect->h,
				CurXOff + SizeColor, pColorRect->y + pColorRect->h);
			Graphics()->QuadsDrawFreeform(&Freeform, 1);
		}

		// yellow to green
		CurXOff += SizeColor;
		{
			Graphics()->SetColor4(
				ColorRGBA(1, 1, 0, 1),
				ColorRGBA(0, 1, 0, 1),
				ColorRGBA(0, 1, 0, 1),
				ColorRGBA(1, 1, 0, 1));

			IGraphics::CFreeformItem Freeform(
				CurXOff, pColorRect->y,
				CurXOff + SizeColor, pColorRect->y,
				CurXOff, pColorRect->y + pColorRect->h,
				CurXOff + SizeColor, pColorRect->y + pColorRect->h);
			Graphics()->QuadsDrawFreeform(&Freeform, 1);
		}

		CurXOff += SizeColor;
		// green to turquoise
		{
			Graphics()->SetColor4(
				ColorRGBA(0, 1, 0, 1),
				ColorRGBA(0, 1, 1, 1),
				ColorRGBA(0, 1, 1, 1),
				ColorRGBA(0, 1, 0, 1));

			IGraphics::CFreeformItem Freeform(
				CurXOff, pColorRect->y,
				CurXOff + SizeColor, pColorRect->y,
				CurXOff, pColorRect->y + pColorRect->h,
				CurXOff + SizeColor, pColorRect->y + pColorRect->h);
			Graphics()->QuadsDrawFreeform(&Freeform, 1);
		}

		CurXOff += SizeColor;
		// turquoise to blue
		{
			Graphics()->SetColor4(
				ColorRGBA(0, 1, 1, 1),
				ColorRGBA(0, 0, 1, 1),
				ColorRGBA(0, 0, 1, 1),
				ColorRGBA(0, 1, 1, 1));

			IGraphics::CFreeformItem Freeform(
				CurXOff, pColorRect->y,
				CurXOff + SizeColor, pColorRect->y,
				CurXOff, pColorRect->y + pColorRect->h,
				CurXOff + SizeColor, pColorRect->y + pColorRect->h);
			Graphics()->QuadsDrawFreeform(&Freeform, 1);
		}

		CurXOff += SizeColor;
		// blue to purple
		{
			Graphics()->SetColor4(
				ColorRGBA(0, 0, 1, 1),
				ColorRGBA(1, 0, 1, 1),
				ColorRGBA(1, 0, 1, 1),
				ColorRGBA(0, 0, 1, 1));

			IGraphics::CFreeformItem Freeform(
				CurXOff, pColorRect->y,
				CurXOff + SizeColor, pColorRect->y,
				CurXOff, pColorRect->y + pColorRect->h,
				CurXOff + SizeColor, pColorRect->y + pColorRect->h);
			Graphics()->QuadsDrawFreeform(&Freeform, 1);
		}

		CurXOff += SizeColor;
		// purple to red
		{
			Graphics()->SetColor4(
				ColorRGBA(1, 0, 1, 1),
				ColorRGBA(1, 0, 0, 1),
				ColorRGBA(1, 0, 0, 1),
				ColorRGBA(1, 0, 1, 1));

			IGraphics::CFreeformItem Freeform(
				CurXOff, pColorRect->y,
				CurXOff + SizeColor, pColorRect->y,
				CurXOff, pColorRect->y + pColorRect->h,
				CurXOff + SizeColor, pColorRect->y + pColorRect->h);
			Graphics()->QuadsDrawFreeform(&Freeform, 1);
		}
	};

	auto &&RenderSaturationRect = [&](CUIRect *pColorRect, const ColorRGBA &CurColor) {
		ColorHSLA LeftColor = color_cast<ColorHSLA>(CurColor);
		ColorHSLA RightColor = color_cast<ColorHSLA>(CurColor);

		LeftColor.s = 0.0f;
		RightColor.s = 1.0f;

		const ColorRGBA LeftColorRGBA = color_cast<ColorRGBA>(LeftColor);
		const ColorRGBA RightColorRGBA = color_cast<ColorRGBA>(RightColor);

		Graphics()->SetColor4(LeftColorRGBA, RightColorRGBA, RightColorRGBA, LeftColorRGBA);

		IGraphics::CFreeformItem Freeform(
			pColorRect->x, pColorRect->y,
			pColorRect->x + pColorRect->w, pColorRect->y,
			pColorRect->x, pColorRect->y + pColorRect->h,
			pColorRect->x + pColorRect->w, pColorRect->y + pColorRect->h);
		Graphics()->QuadsDrawFreeform(&Freeform, 1);
	};

	auto &&RenderLightingRect = [&](CUIRect *pColorRect, const ColorRGBA &CurColor) {
		ColorHSLA LeftColor = color_cast<ColorHSLA>(CurColor);
		ColorHSLA RightColor = color_cast<ColorHSLA>(CurColor);

		LeftColor.l = DarkestLight;
		RightColor.l = 1.0f;

		const ColorRGBA LeftColorRGBA = color_cast<ColorRGBA>(LeftColor);
		const ColorRGBA RightColorRGBA = color_cast<ColorRGBA>(RightColor);

		Graphics()->SetColor4(LeftColorRGBA, RightColorRGBA, RightColorRGBA, LeftColorRGBA);

		IGraphics::CFreeformItem Freeform(
			pColorRect->x, pColorRect->y,
			pColorRect->x + pColorRect->w, pColorRect->y,
			pColorRect->x, pColorRect->y + pColorRect->h,
			pColorRect->x + pColorRect->w, pColorRect->y + pColorRect->h);
		Graphics()->QuadsDrawFreeform(&Freeform, 1);
	};

	auto &&RenderAlphaRect = [&](CUIRect *pColorRect, const ColorRGBA &CurColorFull) {
		const ColorRGBA LeftColorRGBA = color_cast<ColorRGBA>(color_cast<ColorHSLA>(CurColorFull).WithAlpha(0.0f));
		const ColorRGBA RightColorRGBA = color_cast<ColorRGBA>(color_cast<ColorHSLA>(CurColorFull).WithAlpha(1.0f));

		Graphics()->SetColor4(LeftColorRGBA, RightColorRGBA, RightColorRGBA, LeftColorRGBA);

		IGraphics::CFreeformItem Freeform(
			pColorRect->x, pColorRect->y,
			pColorRect->x + pColorRect->w, pColorRect->y,
			pColorRect->x, pColorRect->y + pColorRect->h,
			pColorRect->x + pColorRect->w, pColorRect->y + pColorRect->h);
		Graphics()->QuadsDrawFreeform(&Freeform, 1);
	};

	for(int i = 0; i < 3 + Alpha; i++)
	{
		CUIRect Button, Label;
		pRect->HSplitTop(SizePerEntry, &Button, pRect);
		pRect->HSplitTop(MarginPerEntry, nullptr, pRect);
		Button.VSplitLeft(140.0f, &Label, &Button);
		Label.VMargin(10.0f, &Label);

		Button.Draw(ColorRGBA(0.15f, 0.15f, 0.15f, 1.0f), IGraphics::CORNER_ALL, 1.0f);

		CUIRect Rail;
		Button.Margin(2.0f, &Rail);

		char aBuf[32];

		// Hue
		if(i == 0)
			str_format(aBuf, sizeof(aBuf), "%s: %.1f° (%03d)", apLabels[i], Color[i] * 360.0f, round_to_int(Color[i] * 255.0f));
		// Lht
		else if(i == 2)
		{
			// handle internal light clamping, see `UnclampLighting`
			float Lht = DarkestLight + Color[i] * (1.0f - DarkestLight);
			str_format(aBuf, sizeof(aBuf), "%s: %.1f%% (%03d)", apLabels[i], Lht * 100.0f, round_to_int(Color[i] * 255.0f));
		}
		// Sat and Alpha
		else
			str_format(aBuf, sizeof(aBuf), "%s: %.1f%% (%03d)", apLabels[i], Color[i] * 100.0f, round_to_int(Color[i] * 255.0f));
		Ui()->DoLabel(&Label, aBuf, 12.0f, TEXTALIGN_ML);

		ColorRGBA HandleColor;
		Graphics()->TextureClear();
		Graphics()->TrianglesBegin();
		if(i == 0)
		{
			RenderHueRect(&Rail);
			HandleColor = color_cast<ColorRGBA>(ColorHSLA(Color.h, 1.0f, 0.5f, 1.0f));
		}
		else if(i == 1)
		{
			RenderSaturationRect(&Rail, color_cast<ColorRGBA>(ColorHSLA(Color.h, 1.0f, 0.5f, 1.0f)));
			HandleColor = color_cast<ColorRGBA>(ColorHSLA(Color.h, Color.s, 0.5f, 1.0f));
		}
		else if(i == 2)
		{
			RenderLightingRect(&Rail, color_cast<ColorRGBA>(ColorHSLA(Color.h, Color.s, 0.5f, 1.0f)));
			HandleColor = color_cast<ColorRGBA>(ColorHSLA(Color.h, Color.s, Color.l, 1.0f).UnclampLighting(DarkestLight));
		}
		else if(i == 3)
		{
			RenderAlphaRect(&Rail, color_cast<ColorRGBA>(ColorHSLA(Color.h, Color.s, Color.l, 1.0f).UnclampLighting(DarkestLight)));
			HandleColor = color_cast<ColorRGBA>(Color.UnclampLighting(DarkestLight));
		}
		Graphics()->TrianglesEnd();

		Color[i] = Ui()->DoScrollbarH(&((char *)pColor)[i], &Button, Color[i], &HandleColor);
	}

	if(OriginalColor != Color)
	{
		*pColor = Color.Pack(Alpha);
	}
	return PrevPackedColor != *pColor;
}
