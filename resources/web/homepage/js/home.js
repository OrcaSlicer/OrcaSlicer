//var TestData={"sequence_id":"0","command":"get_recent_projects","response":[{"path":"D:\\work\\Models\\Toy\\3d-puzzle-cube-model_files\\3d-puzzle-cube.3mf","time":"2022\/3\/24 20:33:10"},{"path":"D:\\work\\Models\\Art\\Carved Stone Vase - remeshed+drainage\\Carved Stone Vase.3mf","time":"2022\/3\/24 17:11:51"},{"path":"D:\\work\\Models\\Art\\Kity & Cat\\Cat.3mf","time":"2022\/3\/24 17:07:55"},{"path":"D:\\work\\Models\\Toy\\鐩村墤.3mf","time":"2022\/3\/24 17:06:02"},{"path":"D:\\work\\Models\\Toy\\minimalistic-dual-tone-whistle-model_files\\minimalistic-dual-tone-whistle.3mf","time":"2022\/3\/22 21:12:22"},{"path":"D:\\work\\Models\\Toy\\spiral-city-model_files\\spiral-city.3mf","time":"2022\/3\/22 18:58:37"},{"path":"D:\\work\\Models\\Toy\\impossible-dovetail-puzzle-box-model_files\\impossible-dovetail-puzzle-box.3mf","time":"2022\/3\/22 20:08:40"}]};

var m_HotModelList = null;

// Project folders state
var m_ProjectFolders = [];      // [{name, paths:[...]}]
var m_CurrentFolderName = '';   // name of the open folder, or ''

function OnInit()
{
	//-----Official-----
    TranslatePage();

	SendMsg_GetLoginInfo();
	SendMsg_GetRecentFile();
	SendMsg_GetProjectFolders();
	SendMsg_GetStaffPick();
}

//------最佳打开文件的右键菜单功能----------
var RightBtnFilePath='';

var MousePosX=0;
var MousePosY=0;
var sImages = {};
 
function Set_RecentFile_MouseRightBtn_Event()
{
	$("#FileList .FileItem").mousedown(
		function(e)
		{			
			//FilePath
			RightBtnFilePath=$(this).attr('fpath');
			
			if(e.which == 3){
				ShowRecnetFileContextMenu();
			}else if(e.which == 2){
				//middle button
			}else if(e.which == 1){
				OnOpenRecentFile( encodeURI(RightBtnFilePath) );
			}
		});

	$(document).bind("contextmenu",function(e){
		return false;
	});	
	
    $(document).mousemove( function(e){
		MousePosX=e.pageX;
		MousePosY=e.pageY;
	} );
	

	$(document).click( function(e){
		var elem = e.target || e.srcElement;
		var inRecent = false, inPicker = false, inOther = false;
		var cur = elem;
		while (cur) {
			if (cur.id == 'recnet_context_menu')      { inRecent = true; break; }
			if (cur.id == 'folder_picker_menu')        { inPicker = true; break; }
			if (cur.id == 'folder_context_menu' ||
			    cur.id == 'folder_file_context_menu')  { inOther  = true; break; }
			cur = cur.parentNode;
		}

		if (inPicker || inOther) return; // keep everything open
		if (inRecent) {
			// User clicked a different item in the recent menu — close picker only
			$('#folder_picker_menu').hide();
			return;
		}
		HideAllContextMenus();
	} );
}

function HideAllContextMenus()
{
	$("#recnet_context_menu").hide();
	$("#folder_picker_menu").hide();
	$("#folder_context_menu").hide();
	$("#folder_file_context_menu").hide();
}

function ShowContextMenuAt(menuId)
{
	HideAllContextMenus();
	$(menuId).offset({top: 10000, left:-10000});
	$(menuId).show();
	
	let w = $(menuId).outerWidth();
	let h = $(menuId).outerHeight();
	let docW = $(document).width();
	let docH = $(document).height();
	
	let rx = MousePosX;
	let ry = MousePosY;
	
	if (rx + w + 24 > docW) rx = docW - w - 24;
	if (ry + h + 24 > docH) ry = docH - h - 24;
	
	$(menuId).offset({top: ry, left: rx});
}

function SetLoginPanelVisibility(visible) {
  var leftBoard = document.getElementById("LeftBoard");
  if (visible) {
    leftBoard.style.display = "block";
  } else {
    leftBoard.style.display = "none";
  }
}

function HandleStudio( pVal )
{
	let strCmd = pVal['command'];
	
	if (strCmd == "get_recent_projects") {
		ShowRecentFileList(pVal["response"]);
	} else if (strCmd == "get_project_folders") {
		m_ProjectFolders = pVal["response"] || [];
		ShowFolderList(m_ProjectFolders);
	} else if (strCmd == "studio_userlogin") {
		SetLoginInfo(pVal["data"]["avatar"], pVal["data"]["name"]);
	} else if (strCmd == "studio_useroffline") {
		SetUserOffline();
	} else if (strCmd == "studio_set_mallurl") {
		SetMallUrl(pVal["data"]["url"]);
	} else if (strCmd == "studio_clickmenu") {
		let strName = pVal["data"]["menu"];
		GotoMenu(strName);
	} else if (strCmd == "network_plugin_installtip") {
		let nShow = pVal["show"] * 1;
		if (nShow == 1) {
			$("#NoPluginTip").show();
			$("#NoPluginTip").css("display", "flex");
		} else {
			$("#NoPluginTip").hide();
		}
	} else if (strCmd == "modelmall_model_advise_get") {
		if (m_HotModelList != null) {
			let SS1 = JSON.stringify(pVal["hits"]);
			let SS2 = JSON.stringify(m_HotModelList);
			if (SS1 == SS2) return;
		}
		m_HotModelList = pVal["hits"];
		ShowStaffPick(m_HotModelList);
	} else if (strCmd == "SetLoginPanelVisibility") {
		SetLoginPanelVisibility(pVal["visible"]);
	}
}

function GotoMenu( strMenu )
{
	let MenuList=$(".BtnItem");
	let nAll=MenuList.length;
	
	for(let n=0;n<nAll;n++)
	{
		let OneBtn=MenuList[n];
		
		if( $(OneBtn).attr("menu")==strMenu )
		{
			$(".BtnItem").removeClass("BtnItemSelected");			
			
			$(OneBtn).addClass("BtnItemSelected");
			
			$("div[board]").hide();
			$("div[board=\'"+strMenu+"\']").show();
		}
	}
}

function SetLoginInfo( strAvatar, strName ) 
{
	$("#Login1").hide();
	
	$("#UserName").text(strName);
	
    let OriginAvatar=$("#UserAvatarIcon").prop("src");
	if(strAvatar!=OriginAvatar)
		$("#UserAvatarIcon").prop("src",strAvatar);
	
	$("#Login2").show();
	$("#Login2").css("display","flex");
}

function SetUserOffline()
{
	$("#UserAvatarIcon").prop("src","img/c.jpg");
	$("#UserName").text('');
	$("#Login2").hide();	
	
	$("#Login1").show();
	$("#Login1").css("display","flex");
}

function SetMallUrl( strUrl )
{
	$("#MallWeb").prop("src",strUrl);
}


function ShowRecentFileList( pList )
{
	let nTotal=pList.length;
	
	let strHtml='';
	for(let n=0;n<nTotal;n++)
	{
		let OneFile=pList[n];
		
		let sPath=OneFile['path'];
		let sImg=OneFile["image"] || sImages[sPath];
		let sTime=OneFile['time'];
		let sName=OneFile['project_name'];
		sImages[sPath] = sImg;
		
		let TmpHtml='<div class="FileItem"  fpath="'+sPath+'"  >'+
				'<a class="FileTip" title="'+sPath+'"></a>'+
				'<div class="FileImg" ><img src="'+sImg+'" onerror="this.onerror=null;this.src=\'img/d.png\';"  alt="No Image"  /></div>'+
				'<div class="FileName TextS1">'+sName+'</div>'+
				'<div class="FileDate">'+sTime+'</div>'+
			    '</div>';
		
		strHtml+=TmpHtml;
	}
	
	$("#FileList").html(strHtml);	
	
    Set_RecentFile_MouseRightBtn_Event();
	UpdateRecentClearBtnDisplay();
}

function ShowRecnetFileContextMenu()
{
	$('#CT_AddToFolder_Bar').show();
	ShowContextMenuAt('#recnet_context_menu');
}

/*-------RecentFile MX Message------*/
function SendMsg_GetLoginInfo()
{
	var tSend={};
	tSend['sequence_id']=Math.round(new Date() / 1000);
	tSend['command']="get_login_info";
	
	SendWXMessage( JSON.stringify(tSend) );	
}


function SendMsg_GetRecentFile()
{
	var tSend={};
	tSend['sequence_id']=Math.round(new Date() / 1000);
	tSend['command']="get_recent_projects";
	
	SendWXMessage( JSON.stringify(tSend) );
}


function OnLoginOrRegister()
{
	var tSend={};
	tSend['sequence_id']=Math.round(new Date() / 1000);
	tSend['command']="homepage_login_or_register";
	
	SendWXMessage( JSON.stringify(tSend) );	
}

function OnClickModelDepot()
{
	var tSend={};
	tSend['sequence_id']=Math.round(new Date() / 1000);
	tSend['command']="homepage_modeldepot";
	
	SendWXMessage( JSON.stringify(tSend) );		
}

function OnClickNewProject()
{
	var tSend={};
	tSend['sequence_id']=Math.round(new Date() / 1000);
	tSend['command']="homepage_newproject";
	
	SendWXMessage( JSON.stringify(tSend) );		
}

function OnClickOpenProject()
{
	var tSend={};
	tSend['sequence_id']=Math.round(new Date() / 1000);
	tSend['command']="homepage_openproject";
	
	SendWXMessage( JSON.stringify(tSend) );		
}

function OnOpenRecentFile( strPath )
{
	var tSend={};
	tSend['sequence_id']=Math.round(new Date() / 1000);
	tSend['command']="homepage_open_recentfile";
	tSend['data']={};
	tSend['data']['path']=decodeURI(strPath);
	
	SendWXMessage( JSON.stringify(tSend) );	
}

function OnDeleteRecentFile( )
{
	//Clear in UI
	$("#recnet_context_menu").hide();
	
	let AllFile=$("#FileList .FileItem");
	let nFile=AllFile.length;
	for(let p=0;p<nFile;p++)
	{
		let pp=AllFile[p].getAttribute("fpath");
		if(pp==RightBtnFilePath)
			$(AllFile[p]).remove();
	}	
	
	UpdateRecentClearBtnDisplay();
	
	//Send Msg to C++
	var tSend={};
	tSend['sequence_id']=Math.round(new Date() / 1000);
	tSend['command']="homepage_delete_recentfile";
	tSend['data']={};
	tSend['data']['path']=RightBtnFilePath;
	
	SendWXMessage( JSON.stringify(tSend) );
}

function OnDeleteAllRecentFiles()
{
	$('#FileList').html('');
	UpdateRecentClearBtnDisplay();
	
	var tSend={};
	tSend['sequence_id']=Math.round(new Date() / 1000);
	tSend['command']="homepage_delete_all_recentfile";
	
	SendWXMessage( JSON.stringify(tSend) );
}

function UpdateRecentClearBtnDisplay()
{
    let AllFile=$("#FileList .FileItem");
	let nFile=AllFile.length;	
	if( nFile>0 )
		$("#RecentClearAllBtn").show();
	else
		$("#RecentClearAllBtn").hide();
}

function OnExploreRecentFile( )
{
	var tSend={};
	tSend['sequence_id']=Math.round(new Date() / 1000);
	tSend['command']="homepage_explore_recentfile";
	tSend['data']={};
	tSend['data']['path']=decodeURI(RightBtnFilePath);
	
	SendWXMessage( JSON.stringify(tSend) );	
	
	HideAllContextMenus();
}

function OnLogOut()
{
	var tSend={};
	tSend['sequence_id']=Math.round(new Date() / 1000);
	tSend['command']="homepage_logout";
	
	SendWXMessage( JSON.stringify(tSend) );	
}

function BeginDownloadNetworkPlugin()
{
	var tSend={};
	tSend['sequence_id']=Math.round(new Date() / 1000);
	tSend['command']="begin_network_plugin_download";
	
	SendWXMessage( JSON.stringify(tSend) );		
}

function OutputKey(keyCode, isCtrlDown, isShiftDown, isCmdDown) {
	var tSend = {};
	tSend['sequence_id'] = Math.round(new Date() / 1000);
	tSend['command'] = "get_web_shortcut";
	tSend['key_event'] = {};
	tSend['key_event']['key'] = keyCode;
	tSend['key_event']['ctrl'] = isCtrlDown;
	tSend['key_event']['shift'] = isShiftDown;
	tSend['key_event']['cmd'] = isCmdDown;

	SendWXMessage(JSON.stringify(tSend));
}


/* ============================================================
   PROJECT FOLDERS
   ============================================================ */

var m_RightClickFolderName = '';   // folder right-clicked in the folder list
var m_FolderFilePath       = '';   // file right-clicked inside a folder view

// ---- Request / receive ----

function SendMsg_GetProjectFolders()
{
	var tSend = {};
	tSend['sequence_id'] = Math.round(new Date() / 1000);
	tSend['command'] = "get_project_folders";
	SendWXMessage(JSON.stringify(tSend));
}

// ---- Render: folder card list ----

function ShowFolderList(folders)
{
	m_ProjectFolders = folders || [];

	if (m_ProjectFolders.length === 0) {
		$('#FolderList').html('');
		$('#FolderEmptyHint').show();
	} else {
		$('#FolderEmptyHint').hide();
		let html = '';
		for (let i = 0; i < m_ProjectFolders.length; i++) {
			let f = m_ProjectFolders[i];
			let count = f['paths'] ? f['paths'].length : 0;
			let safeName = $('<div>').text(f['name']).html();
			html += '<div class="FolderCard" fname="' + safeName + '">' +
				'<div class="FolderCardIcon"><img src="img/folder_card.svg" draggable="false" /></div>' +
				'<div class="FolderCardName TextS1">' + safeName + '</div>' +
				'<div class="FolderCardCount FileDate">' + count + ' project' + (count !== 1 ? 's' : '') + '</div>' +
				'</div>';
		}
		$('#FolderList').html(html);
	}

	// Bind events to folder cards
	$('.FolderCard').on('click', function() {
		let name = $(this).attr('fname');
		OnOpenFolder(name);
	});
	$('.FolderCard').on('mousedown', function(e) {
		if (e.which === 3) {
			m_RightClickFolderName = $(this).attr('fname');
			ShowContextMenuAt('#folder_context_menu');
		}
	});

	// If currently viewing a folder that just got updated, refresh the view
	if (m_CurrentFolderName !== '') {
		let found = m_ProjectFolders.find(function(f) { return f['name'] === m_CurrentFolderName; });
		if (found) {
			ShowFolderContent(found);
		} else {
			// Folder was deleted
			OnBackToFolders();
		}
	}
}

// ---- Open a folder (drill-down) ----

function OnOpenFolder(folderName)
{
	let folder = m_ProjectFolders.find(function(f) { return f['name'] === folderName; });
	if (!folder) return;
	m_CurrentFolderName = folderName;
	ShowFolderContent(folder);
}

function ShowFolderContent(folder)
{
	$('#FolderListView').hide();
	$('#FolderContentView').show();
	$('#FolderContentTitle').text(folder['name']);

	let paths = folder['paths'] || [];
	if (paths.length === 0) {
		$('#FolderContentFileList').html('');
		$('#FolderContentEmpty').show();
	} else {
		$('#FolderContentEmpty').hide();
		let html = '';
		for (let i = 0; i < paths.length; i++) {
			let p = paths[i];
			let idx = Math.max(p.lastIndexOf('\\'), p.lastIndexOf('/'));
			let fname = (idx >= 0) ? p.substring(idx + 1) : p;
			let sImg = sImages[p] || 'img/d.png';
			let safePath = $('<div>').text(p).html();
			html += '<div class="FileItem FolderFileItem" fpath="' + safePath + '">' +
				'<a class="FileTip" title="' + safePath + '"></a>' +
				'<div class="FileImg"><img src="' + sImg + '" onerror="this.onerror=null;this.src=\'img/d.png\';" alt="No Image" /></div>' +
				'<div class="FileName TextS1">' + $('<div>').text(fname).html() + '</div>' +
				'</div>';
		}
		$('#FolderContentFileList').html(html);
	}

	// Bind folder-file events
	$('.FolderFileItem').on('click', function() {
		let p = $(this).attr('fpath');
		OnOpenFolderFile(p);
	});
	$('.FolderFileItem').on('mousedown', function(e) {
		if (e.which === 3) {
			m_FolderFilePath  = $(this).attr('fpath');
			RightBtnFilePath  = m_FolderFilePath; // reuse for explore
			ShowContextMenuAt('#folder_file_context_menu');
		}
	});
}

// ---- Back to folder list ----

function OnBackToFolders()
{
	m_CurrentFolderName = '';
	$('#FolderContentView').hide();
	$('#FolderListView').show();
}

// ---- Custom input modal (replaces native prompt()) ----

var _modalResolve = null;
var _modalIsConfirm = false;

function ShowInputModal(title, defaultValue)
{
	return new Promise(function(resolve) {
		_modalResolve = resolve;
		_modalIsConfirm = false;
		$('#InputModalTitle').text(title);
		$('#InputModalField').val(defaultValue || '').show();
		$('#InputModalOverlay').css('display', 'flex');
		// Defer focus so the element is visible first
		setTimeout(function() {
			$('#InputModalField').focus();
			$('#InputModalField')[0].select();
		}, 50);
	});
}

function ShowConfirmModal(message)
{
	return new Promise(function(resolve) {
		_modalResolve = resolve;
		_modalIsConfirm = true;
		$('#InputModalTitle').text(message);
		$('#InputModalField').hide();
		$('#InputModalOverlay').css('display', 'flex');
	});
}

function OnInputModalOK()
{
	var val = _modalIsConfirm ? true : $('#InputModalField').val().trim();
	$('#InputModalField').show();
	$('#InputModalOverlay').hide();
	if (_modalResolve) { _modalResolve(val); _modalResolve = null; }
}

function OnInputModalCancel()
{
	$('#InputModalField').show();
	$('#InputModalOverlay').hide();
	if (_modalResolve) { _modalResolve(_modalIsConfirm ? false : null); _modalResolve = null; }
}

function OnInputModalKeyDown(e)
{
	if (e.key === 'Enter')  { OnInputModalOK();     e.preventDefault(); }
	if (e.key === 'Escape') { OnInputModalCancel(); e.preventDefault(); }
}

function OnInputModalOverlayClick(e)
{
	if (e.target.id === 'InputModalOverlay') OnInputModalCancel();
}

// ---- New Folder ----

async function OnNewFolder()
{
	let name = await ShowInputModal('Enter folder name:');
	if (name === null || name === '') return;
	name = name.trim();

	var tSend = {};
	tSend['sequence_id'] = Math.round(new Date() / 1000);
	tSend['command']     = 'create_project_folder';
	tSend['data']        = { name: name };
	SendWXMessage(JSON.stringify(tSend));
}

// ---- New folder and immediately add current right-clicked file ----

async function OnNewFolderAndAdd()
{
	HideAllContextMenus();
	let name = await ShowInputModal('Enter new folder name:');
	if (name === null || name === '') return;
	name = name.trim();

	// Create folder first
	var tCreate = {};
	tCreate['sequence_id'] = Math.round(new Date() / 1000);
	tCreate['command']     = 'create_project_folder';
	tCreate['data']        = { name: name };
	SendWXMessage(JSON.stringify(tCreate));

	// Then add file (small delay to let C++ process creation first)
	setTimeout(function() {
		var tAdd = {};
		tAdd['sequence_id']       = Math.round(new Date() / 1000);
		tAdd['command']           = 'add_to_project_folder';
		tAdd['data']              = { folder_name: name, path: RightBtnFilePath };
		SendWXMessage(JSON.stringify(tAdd));
	}, 100);
}

// ---- Show "Add to Folder" picker from recent-file context menu ----

function OnShowAddToFolderMenu(e)
{
	if (e) e.stopPropagation();
	// Build picker list
	let html = '';
	for (let i = 0; i < m_ProjectFolders.length; i++) {
		let safeName = $('<div>').text(m_ProjectFolders[i]['name']).html();
		html += '<div class="CT_Item FolderPickerItem" fname="' + safeName + '">' +
			'<div class="CT_Text">' + safeName + '</div>' +
			'</div>';
	}
	$('#FolderPickerList').html(html);

	// Bind click on each folder pick
	$('.FolderPickerItem').off('click').on('click', function() {
		let folderName = $(this).attr('fname');
		OnAddToFolder(folderName);
	});

	// Show picker anchored to the right edge of the recent context menu,
	// keeping the recent menu open so the user can see what they acted on.
	var $recent = $('#recnet_context_menu');
	var $picker = $('#folder_picker_menu');

	// Temporarily show offscreen to measure
	$picker.offset({top: 10000, left: -10000});
	$picker.show();

	var rOff = $recent.offset();
	var rW   = $recent.outerWidth();
	var rH   = $recent.outerHeight();
	var pW   = $picker.outerWidth();
	var pH   = $picker.outerHeight();
	var docW = $(document).width();
	var docH = $(document).height();

	// Prefer right of recent menu; fall back to left if not enough room
	var px = rOff.left + rW + 4;
	if (px + pW + 8 > docW) px = rOff.left - pW - 4;

	// Align top with recent menu; clamp to viewport
	var py = rOff.top;
	if (py + pH + 8 > docH) py = docH - pH - 8;

	$picker.offset({top: py, left: px});
}

// ---- Add recent file to a folder ----

function OnAddToFolder(folderName)
{
	HideAllContextMenus();
	var tSend = {};
	tSend['sequence_id'] = Math.round(new Date() / 1000);
	tSend['command']     = 'add_to_project_folder';
	tSend['data']        = { folder_name: folderName, path: RightBtnFilePath };
	SendWXMessage(JSON.stringify(tSend));
}

// ---- Open a project from folder view ----

function OnOpenFolderFile(strPath)
{
	HideAllContextMenus();
	var tSend = {};
	tSend['sequence_id'] = Math.round(new Date() / 1000);
	tSend['command']     = 'open_folder_project';
	tSend['data']        = { path: strPath || m_FolderFilePath };
	SendWXMessage(JSON.stringify(tSend));
}

// ---- Remove a file from its folder ----

function OnRemoveFromFolder()
{
	HideAllContextMenus();
	var tSend = {};
	tSend['sequence_id'] = Math.round(new Date() / 1000);
	tSend['command']     = 'remove_from_project_folder';
	tSend['data']        = { folder_name: m_CurrentFolderName, path: m_FolderFilePath };
	SendWXMessage(JSON.stringify(tSend));
}

// ---- Rename folder ----

async function OnRenameFolder()
{
	HideAllContextMenus();
	let oldName = m_RightClickFolderName;
	let newName = await ShowInputModal('Rename folder:', oldName);
	if (newName === null || newName === '' || newName === oldName) return;
	newName = newName.trim();

	var tSend = {};
	tSend['sequence_id'] = Math.round(new Date() / 1000);
	tSend['command']     = 'rename_project_folder';
	tSend['data']        = { old_name: oldName, new_name: newName };
	SendWXMessage(JSON.stringify(tSend));
}

// ---- Delete folder ----

async function OnDeleteFolder()
{
	HideAllContextMenus();
	let name = m_RightClickFolderName;
	let confirmed = await ShowConfirmModal('Delete folder "' + name + '"? The projects inside will not be deleted.');
	if (!confirmed) return;

	var tSend = {};
	tSend['sequence_id'] = Math.round(new Date() / 1000);
	tSend['command']     = 'delete_project_folder';
	tSend['data']        = { name: name };
	SendWXMessage(JSON.stringify(tSend));
}

/* ============================================================
   END PROJECT FOLDERS
   ============================================================ */

function OpenWikiUrl( strUrl )
{
	var tSend={};
	tSend['sequence_id']=Math.round(new Date() / 1000);
	tSend['command']="userguide_wiki_open";
	tSend['data']={};
	tSend['data']['url']=strUrl;
	
	SendWXMessage( JSON.stringify(tSend) );	
}

//--------------Staff Pick-------
var StaffPickSwiper=null;
function InitStaffPick()
{
	if( StaffPickSwiper!=null )
	{
		StaffPickSwiper.destroy(true,true);
		StaffPickSwiper=null;
	}	
	
	StaffPickSwiper = new Swiper('#HotModel_Swiper.swiper', {
            slidesPerView : 'auto',
		    spaceBetween: 16,
			navigation: {
				nextEl: '.swiper-button-next',
				prevEl: '.swiper-button-prev',
			},
		    slidesPerView : 'auto',
		    slidesPerGroup : 3
//			autoplay: {
//				delay: 3000,
//				stopOnLastSlide: false,
//				disableOnInteraction: true,
//				disableOnInteraction: false
//			},
//			pagination: {
//				el: '.swiper-pagination',
//			},
//		    scrollbar: {
//                el: '.swiper-scrollbar',
//				draggable: true
//            }
			});
}

function SendMsg_GetStaffPick()
{
	var tSend={};
	tSend['sequence_id']=Math.round(new Date() / 1000);
	tSend['command']="modelmall_model_advise_get";
	
	SendWXMessage( JSON.stringify(tSend) );
	
	setTimeout(SendMsg_GetStaffPick, 3600 * 1000);
}

function ShowStaffPick( ModelList )
{
	let PickTotal=ModelList.length;
	if(PickTotal==0)
	{
		$('#HotModelList').html('');
		$('#HotModelArea').hide();
		
		return;
	}
	
	let strPickHtml='';
	for(let a=0;a<PickTotal;a++)
	{
		let OnePickModel=ModelList[a];
		
		let ModelID=OnePickModel['design']['id'];
		let ModelName=OnePickModel['design']['title'];
		let ModelCover=OnePickModel['design']['cover']+'?image_process=resize,w_200/format,webp';
		
		let DesignerName=OnePickModel['design']['designCreator']['name'];
		let DesignerAvatar=OnePickModel['design']['designCreator']['avatar']+'?image_process=resize,w_32/format,webp';
		
		strPickHtml+='<div class="HotModelPiece swiper-slide"  onClick="OpenOneStaffPickModel('+ModelID+')" >'+
			    '<div class="HotModel_Designer_Info"><img src="'+DesignerAvatar+'" /><span class="TextS2">'+DesignerName+'</span></div>'+
				'	<div class="HotModel_PrevBlock"><img class="HotModel_PrevImg" src="'+ModelCover+'" /></div>'+
				'	<div  class="HotModel_NameText TextS1" title="'+ModelName+'">'+ModelName+'</div>'+
				'</div>';
	}
	
	$('#HotModelList').html(strPickHtml);
	InitStaffPick();
	$('#HotModelArea').show();
}

function OpenOneStaffPickModel( ModelID )
{
	//alert(ModelID);
	var tSend={};
	tSend['sequence_id']=Math.round(new Date() / 1000);
	tSend['command']="modelmall_model_open";
	tSend['data']={};
	tSend['data']['id']=ModelID;
	
	SendWXMessage( JSON.stringify(tSend) );		
}


//---------------Global-----------------
window.postMessage = HandleStudio;

