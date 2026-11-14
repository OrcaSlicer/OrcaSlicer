function CloseCalibrationMenu()
{
	$('#cali_context_menu').hide();
	$('#cali_menu_trigger').attr('aria-expanded', 'false');
}

function Set_CalibrationMenu_Event()
{
	$(document).off("keydown.calibrationMenu").on("keydown.calibrationMenu", function(e) {
		if (e.key === "Escape") {
			if ($('#cali_context_menu').is(':visible')) {
				CloseCalibrationMenu();
				$('#cali_menu_trigger').trigger('focus');
			}
			return;
		}

		if (e.key !== "Enter" && e.key !== " ")
			return;

		let target = $(e.target);
		if (target.is('#cali_menu_trigger, #cali_context_menu [role="menuitem"]')) {
			e.preventDefault();
			target.trigger('click');
		}
	});
}

$(Set_CalibrationMenu_Event);

function OnClickCalibration()
{
	if ($('#cali_context_menu').is(':visible')) {
		CloseCalibrationMenu();
		return;
	}
	ShowCalibrationMenu();
}

function ShowCalibrationMenu()
{
	$("#cali_context_menu").offset({top: 10000, left:-10000});
	$('#cali_context_menu').show();

	let MenuWidth = $('#cali_context_menu').width();
	let MenuHeight = $('#cali_context_menu').height();
	let DocumentWidth = $(document).width();
	let DocumentHeight = $(document).height();

	let Trigger = $('#cali_menu_trigger');
	Trigger.attr('aria-expanded', 'true');
	let RealX = Trigger.offset().left;
	let RealY = Trigger.offset().top + Trigger.height() + 4;

	if (RealX + MenuWidth + 24 > DocumentWidth)
		RealX = Math.max(0, DocumentWidth - MenuWidth - 24);
	if (RealY + MenuHeight + 24 > DocumentHeight)
		RealY = Math.max(0, DocumentHeight - MenuHeight - 24);

	$("#cali_context_menu").offset({top: RealY, left: RealX});
}

function OnSelectCalibrationTest(nKind)
{
	CloseCalibrationMenu();

	var tSend = {};
	tSend['sequence_id'] = Math.round(new Date() / 1000);
	tSend['command'] = "homepage_calibration_test";
	tSend['data'] = {};
	tSend['data']['kind'] = "" + nKind;

	SendWXMessage(JSON.stringify(tSend));
}

function OnClickCalibrationGuide()
{
	CloseCalibrationMenu();
	OpenUrlInLocalBrowser("https://www.orcaslicer.com/wiki/guides/calibration_guide");
}