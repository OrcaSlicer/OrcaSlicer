function OnInit()
{
	TranslatePage();
    OnSelectMenu(1);
	
	RequestProfile();
	
	RequestCustomFilaments();
	//TestCustomFilaments();
	//OnSelectMenu(2);
}

function HandleStudio(pVal)
{
	let strCmd=pVal['command'];
	//alert(strCmd);
	
	if(strCmd=='response_userguide_profile')
	{
		m_ProfileItem=pVal['response'];
		SortUI();
	}
	else if(strCmd=='update_custom_filaments')
	{
		UpdateCustomFilaments( pVal['data'] );
	}
}

function CancelSelect()
{
	var tSend={};
	tSend['sequence_id']=Math.round(new Date() / 1000);
	tSend['command']="user_guide_cancel";
	tSend['data']={};
		
	SendWXMessage( JSON.stringify(tSend) );			
}

function ConfirmSelect()
{
	let bRet=ResponseFilamentResult();
	
	if(bRet)
    {
		var tSend={};
		tSend['sequence_id']=Math.round(new Date() / 1000);
		tSend['command']="user_guide_finish";
		tSend['data']={};
		tSend['data']['action']="finish";
		
		SendWXMessage( JSON.stringify(tSend) );			
	}
}

function OnSelectMenu( nIndex )
{
	switch(nIndex)
	{
		case 1:
			$('#SystemFilamentBtn').addClass('TitleSelected');
			$('#SystemFilamentBtn').removeClass('TitleUnselected');		
			
			$('#CustomFilamentBtn').addClass('TitleUnselected');
			$('#CustomFilamentBtn').removeClass('TitleSelected');	
			
			$('#SystemFilamentsArea').css('display','flex');
			$('#CustomFilamentsArea').css('display','none');
			break;
		case 2:
			$('#CustomFilamentBtn').addClass('TitleSelected');
			$('#CustomFilamentBtn').removeClass('TitleUnselected');
			
			$('#SystemFilamentBtn').addClass('TitleUnselected');
			$('#SystemFilamentBtn').removeClass('TitleSelected');	
			
			$('#CustomFilamentsArea').css('display','flex');
			$('#SystemFilamentsArea').css('display','none');			
			break;
	}
}

function RequestCustomFilaments()
{
	var tSend={};
	tSend['sequence_id']=Math.round(new Date() / 1000);
	tSend['command']="request_custom_filaments";
		
	SendWXMessage( JSON.stringify(tSend) );		
}

function TestCustomFilaments()
{
	let strTest='{"command":"update_custom_filaments","data":[{"id":"P0c71f94","name":"AMOLEN ABS 222"},{"id":"P19cc6c5","name":"PrimaSelect PLA 231654"},{"id":"P93a5c3b","name":"3DJAKE PLA 111"}],"sequence_id":"2000"}';
	let tItem=JSON.parse(strTest);
	
	HandleStudio(tItem);
}

function UpdateCustomFilaments( CFList )
{
	let strHtml='';
	let nTotal=CFList.length;
	
	for(let n=0;n<nTotal;n++)
	{
		let pItem=CFList[n];
		
		let F_id=pItem['id'];
		let F_name=pItem['name'];
		
		let strAdd='<div class="CFilament_Item">'+
			       '<a  class="CFilament_Name" title="'+F_name+'">'+F_name+'</a><img onClick="CFEdit(\''+F_id+'\')" class="CFilament_EditBtn" src="../../image/edit.svg" />'+
		           '</div>';
		
		strHtml+=strAdd;
	}
	
	$('#CFilament_List').html(strHtml);
}

function OnClickCustomFilamentAdd()
{
	//alert('Create New Custom Filament');
	
	var tSend={};
	tSend['sequence_id']=Math.round(new Date() / 1000);
	tSend['command']="create_custom_filament";
		
	SendWXMessage( JSON.stringify(tSend) );		
}

//编辑某一个自定义材料
function CFEdit( fid )
{
	//alert(fid);
	
	var tSend={};
	tSend['sequence_id']=Math.round(new Date() / 1000);
	tSend['command']="modify_custom_filament";
	tSend['id']=fid;
		
	SendWXMessage( JSON.stringify(tSend) );	
}
