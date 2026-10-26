var	pModel              = {};
var	ModelNozzleSelected = {};
let SearchBox;
let $content;

function InitGlobalVariables()
{
	SearchBox = document.querySelector('.searchTerm');
	$content  = $('#Content');
}

function RequestProfile()
{
	var tSend={};
	tSend['sequence_id']=Math.round(new Date() / 1000);
	tSend['command']="request_userguide_profile";
	
	SendWXMessage( JSON.stringify(tSend) );
}

function HandleStudio( pVal )
{
//	alert(strInput);
//	alert(JSON.stringify(strInput));
//	
//	let pVal=IsJson(strInput);
//	if(pVal==null)
//	{
//		alert("Msg Format Error is not Json");
//		return;
//	}
	
	let strCmd=pVal['command'];
	//alert(strCmd);
	
	if(strCmd=='response_userguide_profile')
	{
		HandleModelList(pVal['response']);
	}
	else if(strCmd=='check_new_printers_result')
	{
		let button = document.getElementById("CheckNewPrintersBtn");
		if (button) {
			SetAriaState(button, "aria-disabled", false);
			button.style.opacity = "1";
		}

		let noticeBar = document.getElementById("NoticeBar");
		let noticeText = document.getElementById("NoticeText");
		let hasError = pVal.hasOwnProperty("error");
		noticeBar.classList.toggle("notice-error", hasError);
		if (hasError) {
			noticeBar.textContent = T("error", "Error");
			noticeText.textContent = pVal["error"];
		} else if (pVal["vendors"] && pVal["vendors"].length > 0) {
			noticeBar.textContent = T("new_printers_found", "New printers found");
			noticeText.textContent = T("new_vendors_installed", "New printer vendors installed: %s", pVal["vendors"].join(", "));
		} else if (pVal["declined"]) {
			noticeBar.textContent = T("information", "Information");
			noticeText.textContent = T("vendors_declined", "New printer vendors were found, but installation was cancelled.");
		} else {
			noticeBar.textContent = T("information", "Information");
			noticeText.textContent = T("no_new_printers", "No new printers found.");
		}

		ShowNotice(1);
	}
}

function HandleModelList( pVal )
{
	if( !pVal.hasOwnProperty("model") )
		return;

	pModel=pVal['model'];

	// ORCA ensure list correctly ordered
	pModel = pModel.sort((a, b)=>(a["vendor"].localeCompare(b["vendor"])))
	pModel = [ // move custom printers to top
		...pModel.filter(i=>i.vendor === "Custom"),
		...pModel.filter(i=>i.vendor !== "Custom")
	];
	
	let nTotal=pModel.length;
	let ModelHtml={};
	for(let n=0;n<nTotal;n++)
	{
		let OneModel=pModel[n];
		
		let strVendor=OneModel['vendor'];
		
		//Add Vendor Html Node
		if($(".OneVendorBlock[vendor='"+strVendor+"']").length==0)
		{
			let HtmlNewVendor = CreateVendorBlock(strVendor);
			$('#Content').append(HtmlNewVendor);
		}
		
		let ModelName=OneModel['model'];
		
		//Collect Html Node Nozzel Html
		if( !ModelHtml.hasOwnProperty(strVendor))
			ModelHtml[strVendor]='';
			
		ModelHtml[strVendor] += CreatePrinterBlock(OneModel); // ORCA
	}
	
	//Update Nozzel Html Append
	// ORCA: HandleModelList can now be called more than once per dialog (e.g. after a new vendor
	// is installed via "check for new printers"). pModel always holds the full, current list, so
	// clear each vendor's printer area before repopulating instead of appending on top of a
	// previous render.
	for( let key in ModelHtml )
	{
		$(".OneVendorBlock[vendor='"+key+"'] .PrinterArea").empty().append( ModelHtml[key] );
	}
	
	//Update Checkbox
	for(let m=0;m<nTotal;m++)
	{
		let OneModel=pModel[m];

		let SelectList=OneModel['nozzle_selected'];
		if(SelectList!='') {
			ChooseModel(OneModel['vendor'], OneModel['model']);
		}
	}

	UpdateSidebarVendors();

	// let AlreadySelect=$(".ModelCheckBoxSelected");
	// let nSelect=AlreadySelect.length;
	// if(nSelect==0)
	// {
	//	$("div.OneVendorBlock[vendor='"+BBL+"'] .ModelCheckBox").addClass('ModelCheckBoxSelected');
	// }
	
	TranslatePage();
}

function SetModelSelect(vendor, model, checked) {
	if (!ModelNozzleSelected.hasOwnProperty(vendor) && !checked) {
		return;
	}

	if (!ModelNozzleSelected.hasOwnProperty(vendor) && checked) {
		ModelNozzleSelected[vendor] = {};
	}

	let oVendor = ModelNozzleSelected[vendor];
	if (oVendor.hasOwnProperty(model) || checked) {
		oVendor[model] = checked;
	}

	UpdateVendorCheckbox(vendor)
}

function GetModelSelect(vendor, model) {
	if (!ModelNozzleSelected.hasOwnProperty(vendor)) {
		return false;
	}

	let oVendor = ModelNozzleSelected[vendor];
	if (!oVendor.hasOwnProperty(model)) {
		return false;
	}

	return oVendor[model];
}

// ORCA the selection bar shows the state, the card's checkbox reports it
function SetModelChecked($cbs, checked)
{
	$cbs.toggleClass('ModelCheckBoxSelected', !!checked);
	SetAriaState($cbs.closest('.PrinterBlock'), 'aria-checked', !!checked);
}

function ChooseModel(vendor, ModelName)
{
	let ChooseItem=$(".ModelCheckBox[vendor='"+vendor+"'][model='"+ModelName+"']");

	if(ChooseItem.length > 0) {
		let checked = !ChooseItem.hasClass('ModelCheckBoxSelected');

		SetModelChecked(ChooseItem, checked);
		SetModelSelect(vendor, ModelName, checked);
	}
}

function FilterModelList(keyword) {

	//Save checkbox state
	let ModelSelect = $('.ModelCheckBox');
	for (let n = 0; n < ModelSelect.length; n++) {
		let OneItem = ModelSelect[n];

		let strModel = OneItem.getAttribute("model");

		let strVendor = OneItem.getAttribute("vendor");

		SetModelSelect(strVendor, strModel, $(OneItem).hasClass('ModelCheckBoxSelected'));
	}

	$('.search')[0].setAttribute("hasvalue", keyword ? "1" : "0")

	let nTotal = pModel.length;
	let ModelHtml = {};
	let kwSplit = keyword.toLowerCase().match(/\S+/g) || [];

	$('#Content').empty();
	for (let n = 0; n < nTotal; n++) {
		let OneModel = pModel[n];

		let strVendor = OneModel['vendor'];
		let search = (OneModel['name'] + '\0' + strVendor).toLowerCase();

		if (!kwSplit.every(s => search.includes(s)))
			continue;

		//Add Vendor Html Node
		if ($(".OneVendorBlock[vendor='" + strVendor + "']").length == 0) {
			let HtmlNewVendor = CreateVendorBlock(strVendor);
			$('#Content').append(HtmlNewVendor);
		}

		//Collect Html Node Nozzel Html
		if (!ModelHtml.hasOwnProperty(strVendor))
			ModelHtml[strVendor] = '';
			
		ModelHtml[strVendor] += CreatePrinterBlock(OneModel); // ORCA
	}

	//Update Nozzel Html Append
	for (let key in ModelHtml) {
		let obj = $(".OneVendorBlock[vendor='" + key + "'] .PrinterArea");
		obj.empty();
		obj.append(ModelHtml[key]);
	}

	UpdateSidebarVendors();

	$content.css("padding-right",  $content[0].scrollHeight > $content[0].clientHeight ? "10px" : "20px");

	// let AlreadySelect=$(".ModelCheckBoxSelected");
	// let nSelect=AlreadySelect.length;
	// if(nSelect==0)
	// {
	//	$("div.OneVendorBlock[vendor='"+BBL+"'] .ModelCheckBox").addClass('ModelCheckBoxSelected');
	// }

	TranslatePage();
}

let SearchCountTimer = null;

function textInput(obj) {
	FilterModelList(obj.value);

	// ORCA say how many printers the search leaves, once typing pauses rather than per keystroke
	clearTimeout(SearchCountTimer);
	SearchCountTimer = setTimeout(() => {
		$('#SearchCountText').text(T('count_shown', '%s shown', $('#Content .PrinterBlock').length));
	}, 500);
}

// ORCA the name the vendor's heading and its sidebar entry both show
function VendorDisplayName(vendorName)
{
	let alt = vendorName;
	if( alt == "BBL" )
		alt = "Bambu Lab";
	if( alt == "Custom")
		alt = T("custom_printer", "Custom Printer");
	if( alt == "Other")
		alt = "Orca colosseum";
	return alt;
}

function CreateVendorBlock(vendorName)
{
	let alt = VendorDisplayName(vendorName);

	let titleId = 'VendorTitle_' + vendorName.replace(/[^\w-]/g, '_');
	// a search leaves only the matching printers in the block, and select-all acts on just those
	let selectAll = SearchBox && SearchBox.value.trim()
		? T('select_all_shown_printers', 'Select all shown %s printers', alt)
		: T('select_all_printers', 'Select all %s printers', alt);

	return 	`<div class="OneVendorBlock" Vendor="${vendorName}" role="group" aria-labelledby="${titleId}">` +
			'	<div class="BlockBanner">' +
			`		<h2 id="${titleId}">` + alt + '</h2>' +
			// a label, so a click anywhere on the count toggles the checkbox without a handler of its own
			'		<label class="BannerBtns">'+
			// "3 / 12" reads as two bare numbers, so a hidden twin says what they count
			'			<div class="modelCount" aria-hidden="true"></div>' +
			'			<span class="A11yOnly modelCountText"></span>' +
			`			<input type="checkbox" class="VendorCheckbox" aria-label="${EscAttr(selectAll)}" onChange="ChooseVendor('${vendorName}')"/>`+
			'		</label>'+
			'	</div>' +
			'	<div class="PrinterArea">	' +
			'	</div>' +
			'</div>';
}

let PrinterInfoSeq = 0;   // ids for aria-describedby, unique across search rebuilds

function CreatePrinterBlock(OneModel)
{
	let vendor = OneModel['vendor']
	let vendorName = vendor=="BBL" ? "Bambu Lab" : vendor=="Custom" ? "Generic Printer" : vendor;
	let modelName  = OneModel['name'];

	// Most of it unneeded. this can be applied in profiles
	if( vendor=="Custom")					
		modelName = modelName.split(" ")[1];
	// these uses different case in name; seckit, ratrig, blocks
	else if (modelName.toLowerCase().startsWith(vendorName.toLowerCase()))
		modelName = modelName.slice(vendorName.length);
	// these not matches. have to fix in profiles to reduce conditions in here;
	else if (vendor == "MagicMaker" && modelName.startsWith("MM"))
		modelName = modelName.slice(("MM").length);
	else if (vendor == "OrcaArena")
		modelName = modelName.slice(("Orca Arena").length);
	else if (vendor == "RolohaunDesign" && modelName.startsWith("Rolohaun"))
		modelName = modelName.slice(("Rolohaun").length);

	let infoId  = 'PrinterInfo' + (++PrinterInfoSeq);
	let checked = !!GetModelSelect(vendor, OneModel['model']);

	// ORCA the card is a checkbox named by the model; the hover card describes it
	return	`<div class="PrinterBlock" tabindex="0" role="checkbox" aria-checked="${checked}" aria-label="${EscAttr(modelName.trim())}" aria-describedby="${infoId}" onClick="ChooseModel('${vendor}','${OneModel['model']}')">`+
			'	<div class="PImg">'+
			'		<img class="ModelThumbnail" src="' + OneModel['cover'] + '" alt="" onerror="this.onerror=null;this.src=\'../img/printer-dummy.png\';"/>'+
			'	</div>'+
			'	<div class="PrinterInfoMark" aria-hidden="true">?</div>'+
			`	<div class="PrinterInfo" id="${infoId}" aria-hidden="true">`+
			'		<div class="title">' + T("nozzle", "Nozzle") + '</div>'+
			'		<div class="value">' + OneModel['nozzle_diameter'].replaceAll(";", " · ") + '</div>'+
			'	</div>'+
			'	<div style="display: flex;">'+
			`		<div class="ModelCheckBox${checked ? ' ModelCheckBoxSelected' : ''}" vendor="${vendor}" model="${OneModel['model']}"></div>`+
			'		<div class="PName">'+ modelName +'</div>'+ // ><p>'+ vendorName +'</p>
			'	</div>'+
			'</div>';
}

// ORCA the open sidebar is a modal dialog: the page behind it is inert, so neither Tab nor a
// screen reader's browse mode can wander out of it. Closing it moves focus to Target, else back
// to the button that opened it.
function SetSidebarOpen(nOpen, Target) {
	let sidebar = document.getElementById('SidebarContainer');
	let wasOpen = sidebar.getAttribute('open') == '1';
	for (let el of document.body.children)
		if (el !== sidebar && !el.matches('dialog, script'))
			el.inert = !!nOpen;
	if (!nOpen && wasOpen)
		(Target || document.querySelector('.SidebarBtn')).focus({preventScroll: true});
	// the sidebar goes inert after focus has left it, so focus never sits on a removed node
	sidebar.setAttribute('open', nOpen ? '1' : '0');
	sidebar.inert = !nOpen;
	if (nOpen)
		sidebar.querySelector('.SidebarItem')?.focus();
}

function scrollToVendor(vendor) {
	const el = $(".OneVendorBlock[vendor='"+vendor+"']")[0];
	if (el){
		SetSidebarOpen(0, el.querySelector('.VendorCheckbox'));
		// no smooth scroll: a screen reader can read out what scrolls past
		document.getElementById('Content').scrollTo({top: el.offsetTop});
	}
}

function UpdateSidebarVendors()
{
	let SidebarHTML = "";
	$(`.OneVendorBlock`).each((i, el)=>{
		let vendor = el.getAttribute('vendor');
		UpdateVendorCheckbox(vendor);
		SidebarHTML +=`<li><button type="button" class="SidebarItem" vendor="${vendor}" onclick="scrollToVendor(this.getAttribute('vendor'))">${VendorDisplayName(vendor)}</button></li>`;
	});
	$('#SidebarVendors').html(SidebarHTML)
}

function ChooseVendor(sVendor) { // automatically selects / unselects all
	const $cbs = $(`.OneVendorBlock[vendor='${sVendor}'] .ModelCheckBox`);
	const sel  = $cbs.length && $cbs.not('.ModelCheckBoxSelected').length;

	SetModelChecked($cbs, sel);

	$cbs.each((i, el)=>{SetModelSelect(sVendor, el.getAttribute('model'), sel)});
}

function UpdateVendorCheckbox(sVendor) {
	const $vb  = $(`.OneVendorBlock[vendor='${sVendor}']`);
	const $cbs = $vb.find(`.ModelCheckBox`);
	const $vcb = $vb.find(`.VendorCheckbox`);

	const selCount = $cbs.filter('.ModelCheckBoxSelected').length;
	const allSel   = selCount === $cbs.length && selCount > 0;
	const nonSel   = selCount === 0;

	const mixed = !allSel && !nonSel;
	SetCheckedState($vcb, allSel);
	if ($vcb.prop('indeterminate') !== mixed)
		$vcb.prop('indeterminate', mixed);

	$vb.find(".modelCount").text(selCount + " / " + $cbs.length);
	$vb.find(".modelCountText").text(
		T('count_selected', '%s of %s selected', selCount, $cbs.length));
}

function OnExit()
{	
	let ModelAll={};
	
	let ModelSelect=$(".ModelCheckBoxSelected");
	let nTotal=ModelSelect.length;

	if( nTotal==0 ) {
		ShowPrinterRequired();
		return 0;
	}
	
	for(let n=0;n<nTotal;n++)
	{
	    let OneItem=ModelSelect[n];
		
		let strModel=OneItem.getAttribute("model");
			
		//alert(strModel+strVendor+strNozzel);
		
		if(!ModelAll.hasOwnProperty(strModel))
		{
			//alert("ADD: "+strModel);
			
			ModelAll[strModel]={};
		
			ModelAll[strModel]["model"]=strModel;
		}
	}
		
	var tSend={};
	tSend['sequence_id']=Math.round(new Date() / 1000);
	tSend['command']="save_userguide_models";
	tSend['data']=ModelAll;
	
	SendWXMessage( JSON.stringify(tSend) );

    return nTotal;
}

function OnExitFilter() {
	let nTotal = 0;
	let ModelAll = {};
	for (let vendor in ModelNozzleSelected) {
		for (let model in ModelNozzleSelected[vendor]) {
			if (!ModelNozzleSelected[vendor][model])
				continue;

			if (!ModelAll.hasOwnProperty(model)) {
				//alert("ADD: "+strModel);

				ModelAll[model] = {};

				ModelAll[model]["model"] = model;
			}

			nTotal++;
		}
	}

	var tSend = {};
	tSend['sequence_id'] = Math.round(new Date() / 1000);
	tSend['command'] = "save_userguide_models";
	tSend['data'] = ModelAll;

	SendWXMessage(JSON.stringify(tSend));

	// ORCA say why Next or Confirm did nothing
	if (nTotal == 0)
		ShowPrinterRequired();

	return nTotal;
}

function ShowPrinterRequired()
{
	let noticeBar = document.getElementById("NoticeBar");
	noticeBar.classList.add("notice-error");
	noticeBar.textContent = T("error", "Error");
	document.getElementById("NoticeText").textContent = T("printer_required", "At least one printer must be selected.");
	ShowNotice(1);
}

// SNAPPY SCROLLING WITHOUT LAGS
const SNAP_DELAY			= 600;
const SNAP_DURATION			= 200;
const SNAP_CORR             = 8; // error correction / tolerance

let scrollTimer				= null;
let lastScrollTop			= 0;
let scrollDir				= 'down';
let isSnapping				= false;
let snapRafId				= null;
let lastSnapTarget			= null;
let waitingForUserScroll	= false;

function findSnap(cur, dir) {
	if (lastSnapTarget !== null && Math.abs(cur - lastSnapTarget) < SNAP_CORR) return null;

	const savedScroll = cur;

	$content[0].scrollTop = 0; // Temporarily scroll to 0 so getBoundingClientRect can get absolute positions

	let bcTop = el=>(el.getBoundingClientRect().top);

	const contentTop = bcTop($content[0]);
	const bannerH    = ($content.find('.BlockBanner')[0] || {}).offsetHeight || 0;

	const firstCard  = $content.find('.PrinterBlock')[0];
	const firstArea  = $content.find('.PrinterArea')[0];
	const cardGap    = (firstCard && firstArea) ? (bcTop(firstCard) - bcTop(firstArea)) : 0;

	const candidates = $content.find('.BlockBanner, .PrinterBlock').get();
	if (dir === 'up') candidates.reverse();

	let result = lastSeen = null;

	for (const el of candidates) {
		const snapTo = Math.round(
			el.classList.contains('BlockBanner')
				? (bcTop(el.closest('.OneVendorBlock')) - contentTop)
				: Math.max(0, bcTop(el) - contentTop - bannerH - cardGap)
		);
		if (snapTo != lastSeen){
			lastSeen = snapTo;
			if (dir === 'down' && snapTo > cur + SNAP_CORR) { result = snapTo; break; }
			if (dir === 'up'   && snapTo < cur - SNAP_CORR) { result = snapTo; break; }
		}
	}

	$content[0].scrollTop = savedScroll; // Restore scroll position

	return result;
}

function smoothScrollTo(target) {
	if (snapRafId) {
		cancelAnimationFrame(snapRafId);
		snapRafId = null;
	}

	const el   = $content[0];
	const from = el.scrollTop;
	const dist = target - from;
	const t0   = performance.now();
	const ease = t => t < 0.5 ? 2*t*t : -1 + (4 - 2*t)*t;

	function onDone() {
		el.scrollTop         = target;
		lastScrollTop        = lastSnapTarget = target;
		waitingForUserScroll = true;
		clearTimeout(scrollTimer);
		scrollTimer = null;
		snapRafId   = null;
		isSnapping  = false;
	}

	if (Math.abs(dist) < 2)
		return onDone();

	snapRafId = requestAnimationFrame(function step(now) {
		const p = Math.min((now - t0) / SNAP_DURATION, 1);
		el.scrollTop = from + dist * ease(p);
		if (p < 1)
			snapRafId = requestAnimationFrame(step);
		else
			onDone();
	});
}

function armSnap() {
	waitingForUserScroll = false;
	lastSnapTarget       = null;
}

function initScrollEvents() {
	$content.on('scroll', function() {
		if (isSnapping) return;

		if      (this.scrollTop > lastScrollTop + 1) scrollDir = 'down';
		else if (this.scrollTop < lastScrollTop - 1) scrollDir = 'up';

		lastScrollTop = this.scrollTop;

		if (waitingForUserScroll) return;

		clearTimeout(scrollTimer);
		scrollTimer = setTimeout(()=>{
			if (isSnapping) return;

			const target = findSnap($content[0].scrollTop, scrollDir);
			if (target){
				isSnapping = true;
				smoothScrollTo(target);
			}
		}, SNAP_DELAY);
	});

	let touchY = 0;
	$content[0].addEventListener('touchstart', e => {
		touchY = e.touches[0].clientY;
		armSnap();
	}, { passive: true });

	$content[0].addEventListener('touchmove', e => {
		const dy = touchY - e.touches[0].clientY;
		if (Math.abs(dy) > 3)
			scrollDir = dy > 0 ? 'down' : 'up';
	}, { passive: true });

	// Re-arm snap system on user scroll
	$content[0].addEventListener('wheel', armSnap, { passive: true });

	// Re-arm on after scrollbar usage
	$content[0].addEventListener('pointerdown', e => {
		if (e.target === $content[0])
			armSnap();
	});

	// Re-arm on keyboard scroll or focus changes
	document.addEventListener('keydown', e => {
		if (document.activeElement != SearchBox){
			let scrollKeys = ['ArrowUp','ArrowDown','PageUp','PageDown',' '];
			let hasFocus    = $content[0].contains(document.activeElement);
			if(scrollKeys.includes(e.key) || (hasFocus && e.which == 9))
				armSnap();
		}
	});

	// ORCA unfocus search bar while scrolling and its content empty
	$content[0].addEventListener("scroll", () => {
		if (document.activeElement === SearchBox && SearchBox.value == "")
			SearchBox.blur();
	});
}

document.addEventListener('DOMContentLoaded', initScrollEvents);

// LAYOUT SELECTOR
function LayoutMode(value) {
	let LayoutSelector = document.querySelector('.LayoutSelector > .TabGroup');
	let LayoutBtns     = Array.from(LayoutSelector.children);
	let LayoutTypes    = ["compact-list","compact-cover","large-cover"];

	if($content[0].getAttribute("layout") === value)
		return;

	// find current visible vendor and scroll to it after layout change
	let target = null;
	for (const el of $content.find('.OneVendorBlock')) {
		if (el.getBoundingClientRect().bottom - $content[0].getBoundingClientRect().top >= -1) {
			target = el.getAttribute("vendor");
			break;
		}
	}

	LayoutBtns.forEach(el => el.classList.remove('selected'));
	LayoutBtns[LayoutTypes.indexOf(value)].classList.add('selected');
	LayoutBtns.forEach((el, i) => SetAriaState(el, 'aria-pressed', LayoutTypes[i] === value));
	$content[0].setAttribute("layout", value);

	if (target) scrollToVendor(target);
}

document.addEventListener('DOMContentLoaded', () => {
	LayoutMode("large-cover");
	SetSidebarOpen(0);
});

// KEY EVENTS
function initKeyEvents(closeOnESC) {
	document.onkeydown = function (event) {
		var e = event || window.event || arguments.callee.caller.arguments[0];

		let sidebar = document.getElementById('SidebarContainer');

		if (e.keyCode == 27){
			if(sidebar.getAttribute('open') == "1") { // prefer to close sidebar first if its open
				SetSidebarOpen(0);
			}
			else if (closeOnESC){
				ClosePage();
			}
		}

		// ORCA focus search bar on key input
		// writable character, no modifier, not typing in a field, not Space on a control
		let a = document.activeElement || document.body;
		if (a != SearchBox && e.key.length === 1 && !e.ctrlKey && !e.metaKey && !e.altKey
			&& !a.matches('input, textarea, select')
			&& !(e.key === ' ' && a.matches('button, a, [role], [tabindex]'))) {
			SetSidebarOpen(0, SearchBox); // typing implies leaving the sidebar
			SearchBox.focus();
		}
	};
}
