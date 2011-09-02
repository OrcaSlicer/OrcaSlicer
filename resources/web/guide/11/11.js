var m_OldRegion='';
var m_Region='';

function OnInit()
{
	//let strInput=JSON.stringify(cData);
	//HandleStudio(strInput);
	
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
	let strCmd=pVal['command'];
	//alert(strCmd);
	
	if(strCmd=='response_userguide_profile')
	{
		HandleRegionlList(pVal['response']);
	}
}

function HandleRegionlList(  pItem )
{
	m_OldRegion=pItem['region'];
	
	let nNum=$(".RegionItem[region='"+m_OldRegion+"']").length;
	if( nNum==1 )
		ChooseRegion(m_OldRegion);
}


function ChooseRegion( strRegion )
{
	m_Region=strRegion;
	
	$('.RegionItem').removeClass('RegionSelected');
	$(".RegionItem[region='"+strRegion+"']").addClass('RegionSelected');
}

function GotoPolicyPage()
{
	let ItemSelected=$('.RegionSelected')[0];
	let RegionFinal=$(ItemSelected).attr("region");
	
	var tSend={};
	tSend['sequence_id']=Math.round(new Date() / 1000);
	tSend['command']="save_region";
	tSend['region']=RegionFinal;
	
	SendWXMessage( JSON.stringify(tSend) );
	
	window.location.href="../21/index.html";
}


