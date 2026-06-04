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
	for( let key in ModelHtml )
	{
		$(".OneVendorBlock[vendor='"+key+"'] .PrinterArea").append( ModelHtml[key] );
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

function ChooseModel(vendor, ModelName)
{
	let ChooseItem=$(".ModelCheckBox[vendor='"+vendor+"'][model='"+ModelName+"']");
	
	if(ChooseItem.length > 0) {
		if( $(ChooseItem).hasClass('ModelCheckBoxSelected') )
			$(ChooseItem).removeClass('ModelCheckBoxSelected');
		else
			$(ChooseItem).addClass('ModelCheckBoxSelected');		

		SetModelSelect(vendor, ModelName, $(ChooseItem).hasClass('ModelCheckBoxSelected'));
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

	//Update Checkbox
	ModelSelect = $('.ModelCheckBox');
	for (let n = 0; n < ModelSelect.length; n++) {
		let OneItem = ModelSelect[n];

		let strModel = OneItem.getAttribute("model");
		let strVendor = OneItem.getAttribute("vendor");

		let checked = GetModelSelect(strVendor, strModel);

		if (checked)
			$(OneItem).addClass('ModelCheckBoxSelected');
		else
			$(OneItem).removeClass('ModelCheckBoxSelected');
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

function textInput(obj) {
	FilterModelList(obj.value);
}

function CreateVendorBlock(vendorName)
{
	let alt = vendorName;
	if( alt == "BBL" )
		alt = "Bambu Lab";
	if( alt == "Custom")
		alt = "Custom Printer";
	if( alt == "Other")
		alt = "Orca colosseum";
	
	return 	'<div class="OneVendorBlock" Vendor="' + vendorName + '">' +
			'	<div class="BlockBanner">' +
			'		<a>' + alt + '</a>' +
			'		<div class="BannerBtns" onClick="ChooseVendor('+"\'"+vendorName+"\'"+')">'+
			'			<div class="modelCount"></div>' +
			'			<input type="checkbox" class="VendorCheckbox"/>'+
			'		</div>'+	
			'	</div>' +
			'	<div class="PrinterArea">	' +
			'	</div>' +
			'</div>';
}

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

	return	'<div class="PrinterBlock" onClick="ChooseModel(\''+vendor+'\',\''+OneModel['model']+'\')">'+
			'	<div class="PImg">'+
			'		<img class="ModelThumbnail" src="' + OneModel['cover'] + '" />'+
			'	</div>'+
			'	<div class="PrinterInfoMark">?</div>'+
			'	<div class="PrinterInfo">'+
			'		<div class="title trans">Nozzle</div>'+
			'		<div class="value">' + OneModel['nozzle_diameter'].replaceAll(";", " · ") + '</div>'+
			'	</div>'+
			'	<div style="display: flex;">'+
			'		<div class="ModelCheckBox" vendor="' +vendor+ '" model="'+OneModel['model']+'"></div>'+
			'		<div class="PName">'+ modelName +'</div>'+ // ><p>'+ vendorName +'</p>
			'	</div>'+
			'</div>';
}

function scrollToVendor(vendor) {
	const el = $(".OneVendorBlock[vendor='"+vendor+"']")[0];
	if (el){
		document.getElementById('SidebarContainer').setAttribute('open', '0');
		document.getElementById('Content').scrollTo({top: el.offsetTop, behavior: "smooth"});
	}
}

function UpdateSidebarVendors()
{
	let SidebarHTML = "";
	$(`.OneVendorBlock`).each((i, el)=>{
		UpdateVendorCheckbox(el.getAttribute("vendor"));
		SidebarHTML +=`<div class="SidebarItem" onclick="scrollToVendor(this.textContent)">${el.getAttribute('vendor')}</div>`;
	});
	$('#SidebarVendors').html(SidebarHTML)
}

function ChooseVendor(sVendor) { // automatically selects / unselects all
	const $cbs = $(`.OneVendorBlock[vendor='${sVendor}'] .ModelCheckBox`);
	const sel  = $cbs.length && $cbs.not('.ModelCheckBoxSelected').length;

	sel ? $cbs.addClass('ModelCheckBoxSelected')
		: $cbs.removeClass('ModelCheckBoxSelected');

	$cbs.each((i, el)=>{SetModelSelect(sVendor, el.getAttribute('model'), sel)});
}

function UpdateVendorCheckbox(sVendor) {
	const $vb  = $(`.OneVendorBlock[vendor='${sVendor}']`);
	const $cbs = $vb.find(`.ModelCheckBox`);
	const $vcb = $vb.find(`.VendorCheckbox`);

	const selCount = $cbs.filter('.ModelCheckBoxSelected').length;
	const allSel   = selCount === $cbs.length && selCount > 0;
	const nonSel   = selCount === 0;

	$vcb.prop({checked: allSel , indeterminate: !allSel && !nonSel});

	$vb.find(".modelCount").text(selCount + " / " + $cbs.length);
}

function OnExit()
{	
	let ModelAll={};
	
	let ModelSelect=$(".ModelCheckBoxSelected");
	let nTotal=ModelSelect.length;

	if( nTotal==0 ) {
		ShowNotice(1);
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

	return nTotal;
}

function ShowNotice( nShow )
{
	if(nShow==0) {
		$("#NoticeMask").hide();
		$("#NoticeBody").hide();
	}
	else {
		$("#NoticeMask").show();
		$("#NoticeBody").show();
	}
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
	$content[0].setAttribute("layout", value);

	if (target) scrollToVendor(target);
}

document.addEventListener('DOMContentLoaded', () => LayoutMode("large-cover"));

// KEY EVENTS
function initKeyEvents(closeOnESC) {
	document.onkeydown = function (event) {
		var e = event || window.event || arguments.callee.caller.arguments[0];

		let sidebar = document.getElementById('SidebarContainer');

		if (e.keyCode == 27){
			if(sidebar.getAttribute('open') == "1") { // prefer to close sidebar first if its open
				sidebar.setAttribute('open', '0');
			}
			else if (closeOnESC){
				ClosePage();
			}
		}

		// ORCA focus search bar on key input
		// SearchBox not in focus && writable character && non modifier
		if (document.activeElement != SearchBox && e.key.length === 1 && !e.ctrlKey && !e.metaKey && !e.altKey) {
			SearchBox.focus();
		}

		// Close sidebar on any key input
		sidebar.setAttribute('open', '0');

		//if (window.event) {
		//	try { e.keyCode = 0; } catch (e) { }
		//	e.returnValue = true;
		//}
	};
}
