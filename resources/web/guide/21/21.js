// UNIQUE FUNCTIONS

// Keep in here for future additions
function OnInit()
{
	//let strInput=JSON.stringify(cData);
	//HandleModelList(cData);
	
	TranslatePage();
	RequestProfile();
}
 
function GotoFilamentPage()
{
	let nChoose=OnExitFilter();
	
	if(nChoose>0)
		window.open('../22/index.html','_self');
}
