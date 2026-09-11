// UNIQUE FUNCTIONS

// Keep in here for future additions
function OnInit()
{
	//let strInput=JSON.stringify(cData);
	//HandleModelList(cData);
	
	TranslatePage();
	RequestProfile();
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
	let nChoose=OnExitFilter();
	
	if(nChoose>0)
    {
		var tSend={};
		tSend['sequence_id']=Math.round(new Date() / 1000);
		tSend['command']="user_guide_finish";
		tSend['data']={};
		tSend['data']['action']="finish";
		
		SendWXMessage( JSON.stringify(tSend) );			
	}
}

function CheckForNewPrinters()
{
	var tSend={};
	tSend['sequence_id']=Math.round(new Date() / 1000);
	tSend['command']="check_for_new_printers";
	tSend['data']={};

	var button = document.getElementById("CheckNewPrintersBtn");
	if (button) {
		button.style.pointerEvents = "none";
		button.style.opacity = "0.6";
	}

	SendWXMessage( JSON.stringify(tSend) );
}

function CreateNewPrinter()
{
	var tSend={};
	tSend['sequence_id']=Math.round(new Date() / 1000);
	tSend['command']="user_guide_create_printer";
	tSend['data']={};
		
	SendWXMessage( JSON.stringify(tSend) );			
}
