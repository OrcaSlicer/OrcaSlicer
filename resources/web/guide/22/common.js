var m_ProfileItem;

var FilamentPriority = new Array( "pla","abs","pet","tpu","pc");
var VendorPriority   = new Array("generic");

function RequestProfile()
{
	var tSend={};
	tSend['sequence_id']=Math.round(new Date() / 1000);
	tSend['command']="request_userguide_profile";
	
	SendWXMessage( JSON.stringify(tSend) );
}

function GetFilamentShortname( sName )
{
	let sShort=sName.split('@')[0].trim();
	
	return sShort;
}

function SortUI()
{
	var ModelList=new Array();
	
	let nMode=m_ProfileItem["model"].length;
	for(let n=0;n<nMode;n++)
	{
		let OneMode=m_ProfileItem["model"][n];
		
		if( OneMode["nozzle_selected"]!="" )
			ModelList.push(OneMode);
	}
	
	//model
	let HtmlMode='';
	nMode=ModelList.length;
	for(let n=0;n<nMode;n++)
	{
		let sModel=ModelList[n];	
		/* ORCA use label tag to allow checkbox to toggle when user ckicked to text */
		HtmlMode+='<label><input type="checkbox" mode="'+sModel['model']+'"  nozzle="'+sModel['nozzle_selected']+'"   onChange="MachineClick()" /><span>'+sModel['model']+'</span></label>';
	}
	
	$('#MachineList .CValues').append(HtmlMode);
	SetCheckedState($('#MachineList .CValues input'), true);
	//if(nMode<=1)
	//{
	//	$('#MachineList').hide();
	//}
	
	//Filament - Create sorted array with generic vendor first
	let FilamentArray=new Array();
	let GenericFilamentArray=new Array();
	for( let key in m_ProfileItem['filament'] )
	{
		let OneFila=m_ProfileItem['filament'][key];
		if(OneFila['vendor'].toLowerCase() === 'generic')
			GenericFilamentArray.push({key: key, data: OneFila});
		else
			FilamentArray.push({key: key, data: OneFila});
	}
	// Combine arrays with generic filaments first
	let SortedFilamentArray = GenericFilamentArray.concat(FilamentArray);
	
	let HtmlFilament='';
	let SelectNumber=0;

	var TypeHtmlArray={};
    var VendorHtmlArray={};
	for( let n=0; n<SortedFilamentArray.length; n++ )
	{
		let filamentItem = SortedFilamentArray[n];
		let key = filamentItem.key;
		let OneFila = filamentItem.data;
		
		//alert(JSON.stringify(OneFila));
		
		let fWholeName=OneFila['name'].trim();
		let fShortName=GetFilamentShortname( OneFila['name'] );
		let fVendor=OneFila['vendor'];
		let fType=OneFila['type'];
		let fSelect=OneFila['selected'];
		let fModel=OneFila['models']
		
        let bFind=false;		
		//let bCheck=$("#MachineList input:first").prop("checked");
		if( fModel=='')
		{
			// Orca: hide
			bFind=true;
		}
		else
		{
			//check in modellist		    
		    let nModelAll=ModelList.length;
		    for(let m=0;m<nModelAll;m++)
		    {
	    		let sOne=ModelList[m];
			
				let OneName=sOne['model'];
				let NozzleArray=sOne["nozzle_selected"].split(';');
				
				let nNozzle=NozzleArray.length;
				
				for( let b=0;b<nNozzle;b++ )
				{
					let nowModel= OneName+"++"+NozzleArray[b];
					if(fModel.indexOf(nowModel)>=0)
					{
						bFind=true;
						break;
					}
				}
			}
		}
		
		if(bFind)
		{
			//Type
			let LowType=fType.toLowerCase();
		    if(!TypeHtmlArray.hasOwnProperty(LowType))
		    {
				/* ORCA use label tag to allow checkbox to toggle when user ckicked to text */
			    let HtmlType='<label><input type="checkbox" filatype="'+fType+'" onChange="FilaClick()"   /><span>'+fType+'</span></label>';
			
				TypeHtmlArray[LowType]=HtmlType;
		    }
			
			//Vendor
			let lowVendor=fVendor.toLowerCase();
			if(!VendorHtmlArray.hasOwnProperty(lowVendor))
		    {
				/* ORCA use label tag to allow checkbox to toggle when user ckicked to text */
			    let HtmlVendor='<label><input type="checkbox" vendor="'+fVendor+'"  onChange="VendorClick()" /><span>'+fVendor+'</span></label>';
				
				VendorHtmlArray[lowVendor]=HtmlVendor;
		    }
			
			//Filament
			let pFila=$("#ItemBlockArea input[vendor='"+fVendor+"'][filatype='"+fType+"'][name='"+fShortName+"']");
	        if(pFila.length==0)
		    {
				/* ORCA use label tag to allow checkbox to toggle when user ckicked to text */
			    let HtmlFila='<label class="MItem"><input type="checkbox" onChange="UpdateStats()" vendor="'+fVendor+'"  filatype="'+fType+'" filalist="'+fWholeName+';'+'"  model="'+fModel+'" name="'+fShortName+'" /><span>'+fShortName+'</span></label>';
			
			    $("#ItemBlockArea").append(HtmlFila);
		    } 
			else
			{
				let strModel=pFila.attr("model");
				let strFilalist=pFila.attr("filalist");
				
				if(strModel == '' || fModel == '')
					pFila.attr("model", '');
				else
					pFila.attr("model", strModel+fModel);
					
				pFila.attr("filalist", strFilalist+fWholeName+';');
			}
			
		    if(fSelect*1==1)
			{
				//alert( fWholeName+' - '+fShortName+' - '+fVendor+' - '+fType+' - '+fSelect+' - '+fModel );
					
				$("#ItemBlockArea input[vendor='"+fVendor+"'][filatype='"+fType+"'][name='"+fShortName+"']").prop("checked",true);
				SelectNumber++;
			}
//			else
//				$("#ItemBlockArea input[vendor='"+fVendor+"'][model='"+fModel+"'][filatype='"+fType+"'][name='"+key+"']").prop("checked",false);			
		}
	} 

	//Sort TypeArray
	let TypeAdvNum=FilamentPriority.length;
	for( let n=0;n<TypeAdvNum;n++ )
	{
		let strType=FilamentPriority[n];
		
		if( TypeHtmlArray.hasOwnProperty( strType ) )
		{
			$("#FilatypeList .CValues").append( TypeHtmlArray[strType] );
			delete( TypeHtmlArray[strType] );
		}
	}
    for(let key in TypeHtmlArray )
	{
		$("#FilatypeList .CValues").append( TypeHtmlArray[key] );
	}
	SetCheckedState($("#FilatypeList .CValues input"), true);

	//Sort VendorArray
	let VendorAdvNum=VendorPriority.length;
	for( let n=0;n<VendorAdvNum;n++ )
	{
		let strVendor=VendorPriority[n];
		
		if( VendorHtmlArray.hasOwnProperty( strVendor ) )
		{
			$("#VendorList .CValues").append( VendorHtmlArray[strVendor] );
			delete( VendorHtmlArray[strVendor] );
		}
	}
    for(let key in VendorHtmlArray )
	{
		$("#VendorList .CValues").append( VendorHtmlArray[key] );
	}	
	SetCheckedState($("#VendorList .CValues input"), true);

	if(SelectNumber==0)
		ChooseDefaultFilament();
	
	UpdateStats();
}

function ChooseAllMachine()
{
	let bCheck=$("#MachineList input:first").prop("checked");

	SetCheckedState($("#MachineList input"), bCheck);

	SortFilament();
}

function MachineClick()
{
	let nChecked=$("#MachineList input:gt(0):checked").length
	let nAll    =$("#MachineList input:gt(0)").length

	SetCheckedState($("#MachineList input:first"), nAll==nChecked);

	SortFilament();
}

function ChooseAllFilament()
{
	let bCheck=$("#FilatypeList input:first").prop("checked");
	SetCheckedState($("#FilatypeList input"), bCheck);

	SortFilament();
}


function FilaClick()
{
	let nChecked=$("#FilatypeList input:gt(0):checked").length
	let nAll    =$("#FilatypeList input:gt(0)").length

	SetCheckedState($("#FilatypeList input:first"), nAll==nChecked);

	SortFilament();
}

function ChooseAllVendor()
{
	let bCheck=$("#VendorList input:first").prop("checked");
	SetCheckedState($("#VendorList input"), bCheck);

	SortFilament();
}

function VendorClick()
{
	let nChecked=$("#VendorList input:gt(0):checked").length
	let nAll    =$("#VendorList input:gt(0)").length

	SetCheckedState($("#VendorList input:first"), nAll==nChecked);

	SortFilament();
}

function SortFilament()
{
	let FilaNodes=$("#ItemBlockArea .MItem");
	let nFilament=FilaNodes.length;
	//$("#ItemBlockArea .MItem").hide();
	
	//ModelList
	let pModel=$("#MachineList input:checked");
	let nModel=pModel.length;
	let ModelList=new Array();
	for(let n=0;n<nModel;n++)
	{
		let OneModel=pModel[n];
		
		let mName=OneModel.getAttribute("mode");
		if( mName=='all' )
		{
			continue;
		}
		else
		{
			let mNozzle=OneModel.getAttribute("nozzle");
			let NozzleArray=mNozzle.split(';');
			
			for( let bb=0;bb<NozzleArray.length;bb++ )
			{
				let NewModel='['+mName+'++'+NozzleArray[bb]+']';
			
				ModelList.push( NewModel );
			}
		}
	}
	
	//TypeList
	let pType=$("#FilatypeList input:gt(0):checked");
	let nType=pType.length;
	let TypeList=new Array();
	for(let n=0;n<nType;n++)
	{
		let OneType=pType[n];
		TypeList.push(  OneType.getAttribute("filatype") );
	}	
	
	//VendorList
	let pVendor=$("#VendorList input:gt(0):checked");
	let nVendor=pVendor.length;
	let VendorList=new Array();
	for(let n=0;n<nVendor;n++)
	{
		let OneVendor=pVendor[n];
		VendorList.push(  OneVendor.getAttribute("vendor") );
	}		
	
	//Update Filament UI
	for(let m=0;m<nFilament;m++)
	{
		let OneNode=FilaNodes[m];
		let OneFF=OneNode.getElementsByTagName("input")[0];
		
	    let fModel=OneFF.getAttribute("model");
		let fVendor=OneFF.getAttribute("vendor");
		let fType=OneFF.getAttribute("filatype");
		let fName=OneFF.getAttribute("name");
		
		if(TypeList.in_array(fType) && VendorList.in_array(fVendor))
		{
			let HasModel=false;
			for(let m=0;m<ModelList.length;m++)
			{
				let ModelSrc=ModelList[m];
				
				if( fModel.indexOf(ModelSrc)>=0)
				{
					HasModel=true;
					break;
				}
			}
			
			if(HasModel || fModel=='')
			    $(OneNode).show();
			else
				$(OneNode).hide();
		}
		else{
			$(OneNode).hide();
			//alert(fName) //debug non common filament type
		}
	}

	UpdateStats();
}

function UpdateStats()
{
	let $i             = $("#ItemBlockArea");
	let $allItems      = $i.find(".MItem");
	let $visibleItems  = $i.find(".MItem:visible");
	let $filteredItems = $visibleItems.filter(function() { return $(this).css('position') !== 'absolute'});
	let visibleCount   = Math.min($filteredItems.length, $visibleItems.length);
	let nChecked       = $i.find("input:checked").length;
	let nAll           = $allItems.length;
	let bFiltered      = nAll > visibleCount;

	$(".list-item-count").text(
		nChecked + " / " + nAll +
		(bFiltered ? (" [" + visibleCount + "]") : "") // filtered items
	);

	$("#FilamentCountText").text(
		bFiltered ? T("count_selected_shown", "%s of %s selected, %s shown", nChecked, nAll, visibleCount)
		          : T("count_selected", "%s of %s selected", nChecked, nAll)
	);
}

function SelectAllFilament( nShow )
{
	// ORCA add ability to only select / unselect filted items
	if (document.querySelector('.cbr-filter-bar').value) {
		SetCheckedState($('#ItemBlockArea .MItem:visible input')
			.filter(function() {return $(this).closest('.MItem').css('position') !== 'absolute'}), nShow != 0);
	}
	else {
		SetCheckedState($('#ItemBlockArea .MItem:visible input'), nShow != 0);
	}

	UpdateStats();
}

function ChooseDefaultFilament()
{
	//ModelList
	let pModel=$("#MachineList input:gt(0)");
	let nModel=pModel.length;
	let ModelList=new Array();
	for(let n=0;n<nModel;n++)
	{
		let OneModel=pModel[n];
		ModelList.push(  OneModel.getAttribute("mode") );
	}	
	
	//DefaultMaterialList
	let DefaultMaterialString = "";
	let nMode=m_ProfileItem["model"].length;
	for(let n=0;n<nMode;n++)
	{
		let OneMode=m_ProfileItem["model"][n];
		let ModeName=OneMode['model'];
        let DefaultM=OneMode['materials'];
		
		if( ModelList.indexOf(ModeName)>-1 )	
		{
			DefaultMaterialString+=OneMode['materials']+';';
		}
	}	
	
	let DefaultMaterialArray=DefaultMaterialString.split(';');
	//alert(DefaultMaterialString);
	
	//Filament
	let FilaNodes=$("#ItemBlockArea .MItem");
    let nFilament=FilaNodes.length;
    for(let m=0;m<nFilament;m++)
	{
		let OneNode=FilaNodes[m];
		let OneFF=OneNode.getElementsByTagName("input")[0];

	    let filamentList=GetFilamentShortname(OneFF.getAttribute("filalist"));
		//alert(filamentList);
		let filamentArray=filamentList.split(';')
		
		let HasModel=false;
		let NowFilaLength=filamentArray.length;
		for(let p=0;p<NowFilaLength;p++)
		{
			let NowFila=filamentArray[p];
		
			if( NowFila!='' && DefaultMaterialArray.indexOf(NowFila)>-1)
			{
				HasModel=true;
				break;
			}
		}
			
		SetCheckedState(OneFF, HasModel);
	}
	
	ShowNotice(0);

	UpdateStats();
}

function ResponseFilamentResult()
{
	let FilaSelectedList= $("#ItemBlockArea input:checked");
	let nAll=FilaSelectedList.length;

	if( nAll==0 )
	{
		ShowNotice(1);
		return false;
	}
	
	let FilaArray=new Array();
	for(let n=0;n<nAll;n++)
	{
		let strFilalist=FilaSelectedList[n].getAttribute("filalist");
		if(strFilalist) {
			let filaNames = strFilalist.split(';');
			for(let i=0; i<filaNames.length; i++) {
				let fname = filaNames[i].trim();
				if(fname !== '')
					FilaArray.push(fname);
			}
		}
	}
	
	var tSend={};
	tSend['sequence_id']=Math.round(new Date() / 1000);
	tSend['command']="save_userguide_filaments";
	tSend['data']={};
	tSend['data']['filament']=FilaArray;
		
	SendWXMessage( JSON.stringify(tSend) );
	
	return true;
}

function addClearBtnEvents(el){
	el.addEventListener('click', e => {
		if (el.getAttribute("onclear") == "1") {
			el.value = '';
			el.dispatchEvent(new Event('input', {bubbles: true}));
		}
	});
	el.addEventListener('mousemove', e => {
		const rc = el.getBoundingClientRect();
		const onRight = el.value && (e.clientX - rc.left > rc.width - 32);
		el.setAttribute("onclear", onRight ? "1" : "0");
	});
	el.addEventListener('mouseleave', e => {
		el.setAttribute("onclear", "0");
	});
}

function initInputEvents(){
	document.querySelectorAll('.cbr-search-bar').forEach(searchBar => {
        searchBar.addEventListener('input', function() {
            const search = this.value.trim().toLowerCase(),
            	  list   = this.closest('.cbr-column').querySelector('.cbr-content'),
            	  items  = list.querySelectorAll('label');
            let   hidden = 0;

            items.forEach((item, i) => {
				if(i == 0){
					item.style.display ="block";
					return;
				};
                const text = item.querySelector("span").textContent.toLowerCase();
                const hide = search && !text.includes(search);
                item.style.display = hide ? "none" : "block";
                if(hide) hidden++;
            });

			if(items.length - hidden == 1){
				items[0].style.display = "none";
				hidden++;
			}

            list.querySelector('.cbr-no-items').style.display = (hidden === items.length) ? "block" : "none";
        });
		addClearBtnEvents(searchBar);
	});

	const filterBar         = document.querySelector('.cbr-filter-bar');
	const filterModeFilter  = document.querySelector('.cbr-filter-mode-filter' );
	const filterModeVisible = document.querySelector('.cbr-filter-mode-visible');
	
	filterBar.addEventListener('input', function() {
        const search = this.value.trim().toLowerCase();
        const list   = this.closest('.cbr-column').querySelector('.cbr-content');
        const items  = list.querySelectorAll('label');
        let   hidden = 0;
		
		filterModeFilter.style.display  = search ? "block" : "none";
		filterModeVisible.style.display = search ? "none" : "block";

		const showSel   = search == "::checked";
		const showUnsel = search == "::unchecked";
		
		if(showSel || showUnsel){
			items.forEach(item => {
				const cb   = item.querySelector("input");
				const hide = showSel ? !cb.checked : cb.checked;
				item.style.position = hide ? "absolute" : "unset";
				if(hide) hidden++;
			});
		}
		else {
			items.forEach(item => {
				const text = item.querySelector("span").textContent.toLowerCase();
				const hide = search && !text.includes(search);
				item.style.position = hide ? "absolute" : "unset";
				if(hide) hidden++;
			});
		}
	
        list.querySelector('.cbr-no-items').style.display = (hidden === items.length) ? "block" : "none";

		UpdateStats();
	});
	addClearBtnEvents(filterBar);

	document.querySelector('#filter-tags').addEventListener('change', e => {
		let v = e.target.value;
		filterBar.value = v == "1" ? "::checked" : "::unchecked";
		filterBar.dispatchEvent(new Event('input', {bubbles: true}));
		filterBar.focus();
		e.target.value = 0; // reset back to make dropdown items always selectable
    });
}