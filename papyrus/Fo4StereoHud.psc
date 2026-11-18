; Feeds HP and ammo to the fo4stereo proxy through the F4SE plugin (native fo4stereo_hud.SetHud).
; Attach to a quest in fo4stereo.esp. The plugin stores the values in shared memory for the proxy.
Scriptname Fo4StereoHud extends Quest

Event OnInit()
	StartTimer(0.25, 1)
EndEvent

Event OnTimer(int aiTimerID)
	Actor player = Game.GetPlayer()
	If player
		float hp = player.GetValue(Game.GetHealthAV())
		float maxHp = player.GetBaseValue(Game.GetHealthAV())
		float reserve = -1.0
		Weapon wpn = player.GetEquippedWeapon()
		If wpn
			Form ammoForm = wpn.GetAmmo()
			If ammoForm
				reserve = player.GetItemCount(ammoForm) as float
			EndIf
		EndIf
		fo4stereo_hud.SetHud(hp, maxHp, reserve)
	EndIf
	StartTimer(0.25, 1)
EndEvent
