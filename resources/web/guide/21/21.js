function OnInit()
{
	//let strInput=JSON.stringify(cData);
	//HandleModelList(cData);
	
	TranslatePage();
	
	RequestProfile();
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

function ShowPrinterThumb(pItem, strImg)
{
	$(pItem).attr('src',strImg);
	$(pItem).attr('onerror',null);
}

function ChooseModel( vendor, ModelName )
{
	let ChooseItem=$(".ModelCheckBox[vendor='"+vendor+"'][model='"+ModelName+"']");
	
	if(ChooseItem!=null)
	{
		if( $(ChooseItem).hasClass('ModelCheckBoxSelected') )
			$(ChooseItem).removeClass('ModelCheckBoxSelected');
		else
			$(ChooseItem).addClass('ModelCheckBoxSelected');		

		SetModelSelect(vendor, ModelName, $(ChooseItem).hasClass('ModelCheckBoxSelected'));
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
			let sVV=strVendor;
			if( sVV=="BBL" )
				sVV="Bambu Lab";
			if( sVV=="Custom")
				sVV="Custom Printer";
			if( sVV=="Other")
				sVV="Orca colosseum";

			let HtmlNewVendor='<div class="OneVendorBlock" Vendor="'+strVendor+'">'+
'<div class="BlockBanner">'+
'	<a>'+sVV+'</a>'+
'	<div class="BannerBtns" onClick="ChooseVendor('+"\'"+strVendor+"\'"+')">'+
'		<div class="modelCount"></div>' +
'		<input type="checkbox" class="VendorCheckbox"/>'+
'	</div>'+		
//'	<div class="BannerBtns">'+
//'		<div class="ButtonStyleConfirm ButtonTypeWindow trans" tid="t11" onClick="SelectPrinterAll('+"\'"+strVendor+"\'"+')">all</div>'+
//'		<div class="ButtonStyleRegular ButtonTypeWindow trans" tid="t12" onClick="SelectPrinterNone('+"\'"+strVendor+"\'"+')">none</div>'+
//'	</div>'+
'</div>'+
'<div class="PrinterArea">	'+
'</div>'+
'</div>';
			
			$('#Content').append(HtmlNewVendor);
		}
		
		let ModelName=OneModel['model'];
		
		//Collect Html Node Nozzel Html
		if( !ModelHtml.hasOwnProperty(strVendor))
			ModelHtml[strVendor]='';
			
		ModelHtml[strVendor]+=CreatePrinterBlock(OneModel); // ORCA
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
		if(SelectList!='')
		{
			ChooseModel(OneModel['vendor'], OneModel['model']);
		}
	}	

	const $SidebarVendors = $('#SidebarVendors');
	let SidebarHTML = "";
	$(`.OneVendorBlock`).each((i, el)=>{
		UpdateVendorCheckbox(el.getAttribute("vendor"));
		SidebarHTML +=`<div class="SidebarItem" onclick="scrollToVendor(this.textContent)">${el.getAttribute('vendor')}</div>`;
	});
	$SidebarVendors.html(SidebarHTML)

	// let AlreadySelect=$(".ModelCheckBoxSelected");
	// let nSelect=AlreadySelect.length;
	// if(nSelect==0)
	// {
	//	$("div.OneVendorBlock[vendor='"+BBL+"'] .ModelCheckBox").addClass('ModelCheckBoxSelected');
	// }
	
	TranslatePage();
}

function scrollToVendor(vendor) {
	const el = $(".OneVendorBlock[vendor='"+vendor+"']")[0];
	if (el){
		document.getElementById('SidebarContainer').setAttribute('open', '0');
		document.getElementById('Content').scrollTo({top: el.offsetTop, behavior: "smooth"});
	}
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
			let sVV = strVendor;
			if (sVV == "BBL")
				sVV = "Bambu Lab";
			if (sVV == "Custom")
				sVV = "Custom Printer";
			if (sVV == "Other")
				sVV = "Orca colosseum";

			let HtmlNewVendor = '<div class="OneVendorBlock" Vendor="' + strVendor + '">' +
				'<div class="BlockBanner">' +
				'	<a>' + sVV + '</a>' +
				'	<div class="BannerBtns" onClick="ChooseVendor('+"\'"+strVendor+"\'"+')">'+
				'		<div class="modelCount"></div>' +
				'		<input type="checkbox" class="VendorCheckbox"/>'+
				'	</div>'+	
				//'	<div class="BannerBtns">' +
				//'		<div class="ButtonStyleConfirm ButtonTypeWindow trans" tid="t11" onClick="SelectPrinterAll(' + "\'" + strVendor + "\'" + ')">all</div>' +
				//'		<div class="ButtonStyleRegular ButtonTypeWindow trans" tid="t12" onClick="SelectPrinterNone(' + "\'" + strVendor + "\'" + ')">none</div>' +
				//'	</div>' +
				'</div>' +
				'<div class="PrinterArea">	' +
				'</div>' +
				'</div>';

			$('#Content').append(HtmlNewVendor);
		}

		//Collect Html Node Nozzel Html
		if (!ModelHtml.hasOwnProperty(strVendor))
			ModelHtml[strVendor] = '';

		ModelHtml[strVendor]+=CreatePrinterBlock(OneModel); // ORCA
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

	const $SidebarVendors = $('#SidebarVendors');
	let SidebarHTML = "";
	$(`.OneVendorBlock`).each((i, el)=>{
		UpdateVendorCheckbox(el.getAttribute("vendor"));
		SidebarHTML +=`<div class="SidebarItem" onclick="scrollToVendor(this.textContent)">${el.getAttribute('vendor')}</div>`;
	});
	$SidebarVendors.html(SidebarHTML)

	const $content = $('#Content');
	$content.css("padding-right",  $content[0].scrollHeight > $content[0].clientHeight ? "10px" : "20px");

	// let AlreadySelect=$(".ModelCheckBoxSelected");
	// let nSelect=AlreadySelect.length;
	// if(nSelect==0)
	// {
	//	$("div.OneVendorBlock[vendor='"+BBL+"'] .ModelCheckBox").addClass('ModelCheckBoxSelected');
	// }

	TranslatePage();
}

function CreatePrinterBlock(OneModel)
{
	// ORCA use single functuon to create blocks to simplify code
	let vendor = OneModel['vendor']
	vendorName = vendor=="BBL" ? "Bambu Lab" : vendor=="Custom" ? "Generic Printer" : vendor;

	let modelName = OneModel['name'];
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

	return '<div class="PrinterBlock" onClick="ChooseModel(\''+vendor+'\',\''+OneModel['model']+'\')">'+
	'<div class="PImg">'+
	'<img class="ModelThumbnail" src="' + OneModel['cover'] + '" />'+
	'</div>'+
	'<div class="PrinterInfoMark">?</div>'+
	'<div class="PrinterInfo">'+
	//'	<div class="title trans">Print volume</div>'+
	//'	<div class="value">' + OneModel['printable_height'] + '</div>'+
	'	<div class="title trans">Nozzle</div>'+
	'	<div class="value">' + OneModel['nozzle_diameter'].replaceAll(";", " · ") + '</div>'+
	'</div>'+
	'<div style="display: flex;">'+
	'	<div class="ModelCheckBox" vendor="' +vendor+ '" model="'+OneModel['model']+'"></div>'+
	'	<div class="PName">'+ modelName +'</div>'+ // ><p>'+ vendorName +'</p>
	'</div>'+
	'</div>';
}

function SelectPrinterAll( sVendor )
{
	$("div.OneVendorBlock[vendor='"+sVendor+"'] .ModelCheckBox").addClass('ModelCheckBoxSelected');
	$("div.OneVendorBlock[vendor='"+sVendor+"'] .ModelCheckBox").each(function() {
    	let strModel = this.getAttribute("model");
		SetModelSelect(sVendor, strModel, true);
	});
}


function SelectPrinterNone( sVendor )
{
	$("div.OneVendorBlock[vendor='"+sVendor+"'] .ModelCheckBox").removeClass('ModelCheckBoxSelected');
	$("div.OneVendorBlock[vendor='"+sVendor+"'] .ModelCheckBox").each(function() {
    	let strModel = this.getAttribute("model");
		SetModelSelect(sVendor, strModel, false);
	});
}

function ChooseVendor(sVendor) {
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

function GotoFilamentPage()
{
	let nChoose=OnExitFilter();
	
	if(nChoose>0)
		window.open('../22/index.html','_self');
}

function OnExitFilter() {

	let nTotal = 0;
	let ModelAll = {};
	for (vendor in ModelNozzleSelected) {
		for (model in ModelNozzleSelected[vendor]) {
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

//
function OnExit()
{	
	let ModelAll={};
	
	let ModelSelect=$(".ModelCheckBoxSelected");
	let nTotal=ModelSelect.length;

	if( nTotal==0 )
	{
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


function ShowNotice( nShow )
{
	if(nShow==0)
	{
		$("#NoticeMask").hide();
		$("#NoticeBody").hide();
	}
	else
	{
		$("#NoticeMask").show();
		$("#NoticeBody").show();
	}
}




