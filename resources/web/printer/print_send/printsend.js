// Vue.js refactored version of printsend.js
const { createApp, unref } = Vue;
const { ElInput, ElButton, ElPopover, ElLoading } = ElementPlus;

const PrintSendApp = {
    data() {
        return {
            // Print  data

            printInfo: {
                modelName: '',
                printTime: '00:00:00',
                totalWeight: 0,
                layerCount: 0,
                thumbnail: '',
                timeLapse: false,
                heatedBedLeveling: false,
                uploadAndPrint: false,
                switchToDeviceTab: false,
                autoRefill: false,
                bedType: 'btPTE',
                currentProjectPrinterModel: '',
                filamentList: []
            },

            // MMS info (separated from printInfo for independent loading)
            mmsInfo: {
                mmsSystemName: '',
                mmsList: [
                    {
                        mmsId: '',
                        mmsName: '',
                        trayList: [
                            {
                                trayId: '',
                                trayName: '',
                                filamentType: '',
                                filamentName: '',
                                filamentColor: '',
                            }
                        ]
                    }
                ]
            },

            // Printer management
            printerList: [],
            curPrinter: null,

            // Bed type management
            bedTypes: [
                { value: 'btPTE', name: this.$t ? this.$t('printSend.texturedA') : 'Textured A', icon: 'img/bed_pte_a.png' },
                { value: 'btPC', name: this.$t ? this.$t('printSend.smoothB') : 'Smooth B', icon: 'img/bed_pei_b.png' }
            ],
            selectedBedType: null,
            hasMmsInfo: false,
            // Filament management
            currentPage: 1,
            pageSize: 5,
            // UI state
            isEditingName: false,
            showFilamentSection: false,
            isIniting: false
        };
    },

    computed: {

        modelImageSrc() {
            return this.printInfo.thumbnail
                ? `data:image/png;base64,${this.printInfo.thumbnail}`
                : '';
        },

        showBedDropdown() {
            return this.printInfo.uploadAndPrint;
        },

        showPrintOptions() {
            return this.printInfo.uploadAndPrint;
        },

        totalPages() {
            return Math.ceil(((this.printInfo.filamentList && this.printInfo.filamentList.length) || 0) / this.pageSize);
        },

        currentPageFilaments() {
            if (!this.printInfo.filamentList) return [];
            const start = (this.currentPage - 1) * this.pageSize;
            const end = Math.min(start + this.pageSize, this.printInfo.filamentList.length);
            return this.printInfo.filamentList.slice(start, end).map((filament) => ({
                ...filament
            }));
        },

        mmsFilamentList() {
            return (this.mmsInfo && this.mmsInfo.mmsList) || [];
        },

        networkPrinters() {
            return this.printerList.filter(printer => printer.networkType === 1);
        },

        localPrinters() {
            return this.printerList.filter(printer => printer.networkType === 0);
        },

        // Check if selected printer model does not match the current project printer model
        printerModelNotMatch() {
            return this.curPrinter && this.printInfo.currentProjectPrinterModel && this.curPrinter.printerModel !== this.printInfo.currentProjectPrinterModel;
        },

        // Check if printer is busy (not idle and not print completed)
        printerBusy() {
            if (!this.curPrinter) return false;
            const status = this.curPrinter.printerStatus;
            if(this.curPrinter.connectStatus !== 1) return true;// If not connected, consider busy
            // Status 0 = idle, Status 16 = print completed
            // Any other status means the printer is busy
            return status !== 0 && status !== 16;
        },

        // Check if printer is in print completed state
        printerPrintCompleted() {
            if (!this.curPrinter) return false;
            // Status 16 = print completed
            return this.curPrinter.printerStatus === 16;
        },

        // Computed property to generate error list
        errorList() {
            const errors = [];
            
            // Priority 1: Printer model mismatch
            if (this.printerModelNotMatch) {
                errors.push(this.$t('printSend.printerModelNotMatch'));
            }
            
            // Priority 2: Printer busy warning (non-idle and non-completed)
            if (this.printerBusy) {
                errors.push(this.$t('printSend.printerBusyWarning'));
            }
            
            // Priority 3: Print completed warning (only if "Upload and Print" is checked)
            if (this.printerPrintCompleted && this.printInfo.uploadAndPrint) {
                errors.push(this.$t('printSend.printCompleteWarning'));
            }
            
            return errors;
        }
    },

    methods: {
        // Lifecycle methods
        async init() {
            this.isIniting = true;
            await this.requestPrinterList();
            this.isIniting = false;
        },

        // IPC Communication methods
        async ipcRequest(method, params = {}, timeout = 10000) {
            try {
                const response = await nativeIpc.request(method, params, timeout);
                return response;
            } catch (error) {
                console.error(`IPC request failed for ${method}:`, error);
                this.showStatusTip(error.message || 'Request failed');
                throw error;
            }
        },


        // Communication with backend
        async requestPrintTask() {
            const loading = ElLoading.service({
                lock: true,
            });
            try {
                const params = {
                    printerId: this.curPrinter.printerId
                };
                const response = await this.ipcRequest('request_print_task', params);
                if (response && response.modelName) {
                    response.modelName = this.filterModelName(response.modelName);
                }
                this.printInfo = { ...this.printInfo, ...response };
            } catch (error) {
                // Failure: set to null to indicate request failed (not a valid state)
                this.printInfo = null;
                console.error('Failed to request print task:', error);
            } finally {
                loading.close();
            }
        },

        async requestMmsInfo() {         
            const loading = ElLoading.service({
                lock: true,
            });
            try {  
                const params = {
                    printerId: this.curPrinter.printerId
                };
                const response = await this.ipcRequest('request_mms_info', params);
                if(!response) {
                    this.mmsInfo = null;
                    this.printInfo.filamentList = [];
                    return;
                }
                this.mmsInfo = response.mmsInfo || null;
                this.printInfo.filamentList = response.mappedFilamentList || [];        
            } catch (error) {
                console.error('Failed to request MMS info:', error);
                // Failure: set to null to indicate request failed (not a valid state)
                this.mmsInfo = null;
            } finally {
                loading.close();
            }
        },
        async requestPrinterList() {
            // const loading = ElLoading.service({
            //     lock: true,
            // });
            try {
                const response = await this.ipcRequest('request_printer_list', {});
                this.printerList = response || [];
                await this.updatePrinterSelection();
            } catch (error) {
                console.error('Failed to request printer list:', error);
            } finally {
                // loading.close();
            }
        },

        async resizeWindow() {
            let expand = false;
            if (this.printInfo && this.printInfo.uploadAndPrint &&
                this.mmsInfo &&
                this.mmsInfo.mmsList &&
                this.mmsInfo.mmsList.length > 0
            ) {
                expand = true;
            }
            try {
                nativeIpc.sendEvent('expand_window', { expand });
            } catch (error) {
                console.error('Failed to request printer list:', error);
            }
        },

        async refreshPrinterList(event, value) {
            if (event) {
                event.stopPropagation();
            }
            await this.requestPrinterList();
        },

        async updatePrinterSelection() {
            if (this.printerList.length === 0) return;
            let selectedPrinter;
            if (this.curPrinter && this.curPrinter.printerId) {
                // find the printer by printerId
                let cur = this.printerList.find(p => p.printerId === this.curPrinter.printerId);
                if (cur) {
                    selectedPrinter = cur;
                }
            }
            // if not found, find the selected printer
            if (!selectedPrinter) {
                selectedPrinter = this.printerList.find(p => p.selected);
                if (!selectedPrinter) {
                    selectedPrinter = this.printerList[0];
                }
            }
            this.curPrinter = selectedPrinter;
            await this.onPrinterChanged();
        },

        // Model name editing
        startEditing() {
            this.isEditingName = true;
            this.$nextTick(() => {
                this.$refs.modelNameInput.focus();
            });
        },

        stopEditing() {
            this.isEditingName = false;
            this.printInfo.modelName = this.filterModelName(this.printInfo.modelName);
            this.adjustModelNameWidth();
        },

        onModelNameInput(value) {
            const filtered = this.filterModelName(value);
            if (filtered !== value) {
                this.printInfo.modelName = filtered;
            }
            this.adjustModelNameWidth();
        },

        filterModelName(str) {
            if (!str) return '';
            // Remove characters not allowed in Windows filenames: \ / : * ? " < > |
            let filtered = str.replace(/[\\/:*?"<>|]/g, '');
            // Windows reserved device names
            const reservedNames = /^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])$/i;
            if (reservedNames.test(filtered)) {
                filtered = filtered + '_';
            }
            // Remove trailing spaces and dots (not allowed in Windows filenames)
            filtered = filtered.replace(/[\s.]+$/, '');
            return filtered;
        },

        adjustModelNameWidth() {
            const modelNameInput = this.$refs.modelNameInput;
            if (!modelNameInput) return;
            const input = modelNameInput.input;
            const span = document.createElement('span');
            span.style.visibility = 'hidden';
            span.style.position = 'fixed';
            span.style.font = window.getComputedStyle(input).font;
            span.innerText = modelNameInput.ref.value || modelNameInput.ref.placeholder;
            document.body.appendChild(span);
            input.style.width = (span.offsetWidth + 20) + 'px';
            document.body.removeChild(span);
        },

        // Filament management
        prevPage() {
            if (this.currentPage > 1) {
                this.currentPage--;
            }
        },

        nextPage() {
            if (this.currentPage < this.totalPages) {
                this.currentPage++;
            }
        },

        getFilamentStyle(filament) {
            const color = filament.filamentColor || '#888';
            const textColor = this.getContrastColor(color);
            return {
                background: color,
                color: textColor,
            };
        },

        getFilamentSvg(filament) {
            const color = filament.filamentColor || '#888';
            return getFilamentSvg(color);
        },

        getMmsCardStyle(mmsFilament) {
            const color = mmsFilament.filamentColor || '#888';
            const textColor = this.getContrastColor(color);
            return {
                background: color,
                color: textColor,
                border: `1px solid ${textColor}`
            };
        },

        getContrastColor(hexColor) {
            hexColor = hexColor.replace('#', '');
            if (hexColor.length === 3) {
                hexColor = hexColor.split('').map(x => x + x).join('');
            }
            const r = parseInt(hexColor.substr(0, 2), 16);
            const g = parseInt(hexColor.substr(2, 2), 16);
            const b = parseInt(hexColor.substr(4, 2), 16);
            const brightness = (r * 299 + g * 587 + b * 114) / 1000;
            return brightness > 180 ? '#222' : '#fff';
        },

        standardizeFilamentName(name, vendor = '') {
            let standardized = (name || '').toUpperCase().trim();
            const vendorUpper = (vendor || '').toUpperCase().trim();

            if (vendorUpper && standardized.includes(vendorUpper)) {
                standardized = standardized.split(vendorUpper).join('').trim();
            }
            if (standardized.includes('GENERIC')) {
                standardized = standardized.split('GENERIC').join('').trim();
            }

            const atPos = standardized.indexOf('@');
            if (atPos !== -1) {
                standardized = standardized.substring(0, atPos).trim();
            }

            standardized = standardized.replace(/-/g, ' ').trim();
            return standardized;
        },

        // MMS popover methods  
        updateFilamentMapping(filamentIndex, tray) {
            const filamentSection = document.getElementsByClassName('filament-section');
            if (filamentSection && filamentSection[0]) {
                filamentSection[0].click();
            }
            console.log('Selected MMS Tray:', tray);
            for (let i = 0; i < this.printInfo.filamentList.length; i++) {
                if (this.printInfo.filamentList[i].index === filamentIndex) {
                    const filament = this.printInfo.filamentList[i]; 
                    const filamentType = (filament.filamentType || '').toUpperCase().trim();
                    const trayFilamentType = (tray.filamentType || '').toUpperCase().trim();
                    const filamentVendor = filament.vendor || '';
                    const trayVendor = tray.vendor || '';
                    const filamentName = this.standardizeFilamentName(filament.filamentName, filamentVendor);
                    const trayFilamentName = this.standardizeFilamentName(tray.filamentName, trayVendor);
                    if ((filamentType !== trayFilamentType && trayFilamentType !== '') &&
                        (filamentName !== trayFilamentName && trayFilamentName !== '')) {
                        this.showStatusTip(this.$t('printSend.filamentTypeNotMatch'));
                        return;
                    }
                    filament.mappedMmsFilament.filamentColor = tray.filamentColor;
                    filament.mappedMmsFilament.filamentName = tray.filamentName;
                    filament.mappedMmsFilament.filamentType = tray.filamentType;
                    filament.mappedMmsFilament.trayName = tray.trayName;
                    filament.mappedMmsFilament.mmsId = tray.mmsId;
                    filament.mappedMmsFilament.trayId = tray.trayId;
                    filament.mappedMmsFilament.filamentWeight = tray.filamentWeight;
                    filament.mappedMmsFilament.filamentDensity = tray.filamentDensity;
                    filament.mappedMmsFilament.minNozzleTemp = tray.minNozzleTemp;
                    filament.mappedMmsFilament.maxNozzleTemp = tray.maxNozzleTemp;
                    filament.mappedMmsFilament.minBedTemp = tray.minBedTemp;
                    filament.mappedMmsFilament.maxBedTemp = tray.maxBedTemp;
                    filament.mappedMmsFilament.status = tray.status;
                    filament.mappedMmsFilament.filamentDiameter = tray.filamentDiameter;
                    filament.mappedMmsFilament.filamentId = tray.filamentId;
                    filament.mappedMmsFilament.vendor = tray.vendor;
                    filament.mappedMmsFilament.serialNumber = tray.serialNumber;
                    filament.mappedMmsFilament.settingId = tray.settingId;
                    filament.mappedMmsFilament.from = tray.from;
                    break;
                }
            }

        },

        // Action methods
        async cancel() {
            try {
                nativeIpc.sendEvent('cancel_print', {});
            } catch (error) {
                console.error('Failed to cancel print:', error);
            }
        },

        async upload() {
            if(this.curPrinter.connectStatus !== 1) {
                this.showStatusTip(this.$t('printSend.printerNotConnected'));
                return;
            }
            // Check if print task data is available
            if(this.printInfo === null) {
                this.showStatusTip(this.$t('printSend.printerNotConnected'));
                return;
            }
            //Validate filament mapping if MMS is present
            if (this.printInfo.uploadAndPrint && this.curPrinter.systemCapabilities.supportsMultiFilament && this.mmsInfo === null) {
                this.showStatusTip(this.$t('printSend.filamentError'));
                return;
            }             
            // Update task with current UI state
            this.printInfo.selectedPrinterId = this.curPrinter.printerId;
            this.printInfo.bedType = this.selectedBedType ? this.selectedBedType.value : 'btPTE';

            if (this.printInfo.uploadAndPrint && this.hasMmsInfo && !this.checkFilamentMapping()) {
                this.showStatusTip(this.$t('printSend.someFilamentsNotMapped'));
                return;
            }

            const loading = ElLoading.service({
                lock: true,
            });

            try {
                const uploadData = {
                    ...this.printInfo,
                };
                await this.ipcRequest('start_upload', uploadData);
            } catch (error) {
                console.error('Failed to start upload:', error);
            } finally {
                loading.close();
            }
        },

        checkFilamentMapping() {
            return this.printInfo.filamentList.every(filament =>
                filament.mappedMmsFilament &&
                filament.mappedMmsFilament.trayName.trim() !== "" &&
                filament.mappedMmsFilament.filamentName.trim() !== "" &&
                filament.mappedMmsFilament.filamentType.trim() !== "" &&
                filament.mappedMmsFilament.mmsId.trim() !== "" &&
                filament.mappedMmsFilament.trayId.trim() !== ""
            );
        },

        // Utility methods
        formatWeight(weight) {
            return weight ? `${weight.toFixed(2)}g` : '0.0g';
        },

        showStatusTip(message) {
            if (window.ElementPlus && window.ElementPlus.ElMessage) {
                window.ElementPlus.ElMessage.error({
                    message: message,
                    duration: 5000,
                    showClose: true
                });
            }
        },

        // Printer status display methods (using shared utilities)
        canShowProgressText(printerStatus, connectStatus) {
            return PrinterStatusUtils.canShowProgressText(printerStatus, connectStatus);
        },

        getPrinterStatus(printerStatus, connectStatus) {
            return PrinterStatusUtils.getPrinterStatus(printerStatus, connectStatus, this.$t);
        },

        getPrinterStatusStyle(printerStatus, connectStatus) {
            return PrinterStatusUtils.getPrinterStatusStyle(printerStatus, connectStatus);
        },

        shouldShowWarningIcon(printerStatus, connectStatus) {
            return PrinterStatusUtils.shouldShowWarningIcon(printerStatus, connectStatus);
        },

        getWarningTooltip(printerStatus) {
            return PrinterStatusUtils.getWarningTooltip(printerStatus, this.$t);
        },

        getPrinterProgress(printer) {
            return PrinterStatusUtils.getPrinterProgress(printer);
        },

        getPrinterRemainingTime(printer) {
            return PrinterStatusUtils.getPrinterRemainingTime(printer);
        },

        // Event handlers called by external code
        async onPrinterChanged() {
            // Handle printer change logic if needed
            await this.requestPrintTask();

            if(this.curPrinter.systemCapabilities.supportsMultiFilament) {
               await this.requestMmsInfo();
            } 
         console.log("Printer changed to:", this.curPrinter);
            // Set bed type if provided
            if (this.printInfo.bedType) {
                const bedType = this.bedTypes.find(b => b.value === this.printInfo.bedType);
                this.selectedBedType = bedType || this.bedTypes[0];
            }

            // Check for MMS info
            if (this.curPrinter.systemCapabilities.supportsMultiFilament &&
                this.mmsInfo &&
                this.mmsInfo.connected &&
                this.mmsInfo.mmsList &&
                this.mmsInfo.mmsList.length > 0) {
                this.hasMmsInfo = true;
            } else {
                this.hasMmsInfo = false;
            }
            this.refreshShowFilamentSection();
            this.$nextTick(() => {
                this.adjustModelNameWidth();
            });
        },

        onBedTypeChanged(bedType) {
            // Handle bed type change logic if needed
            console.log("Selected bed type:", bedType);
        },

        refreshShowFilamentSection() {
            setTimeout(() => {
                this.showFilamentSection = this.printInfo.uploadAndPrint && this.hasMmsInfo;
            }, this.printInfo.uploadAndPrint && this.hasMmsInfo ? 100 : 0);
        }
    },

    mounted() {
        this.selectedBedType = this.bedTypes[0];

        // Initialize the application
        this.init();
        disableRightClickMenu();
    },

    watch: {
        'mmsInfo.mmsList': {
            async handler(newValue, oldValue) {
                this.resizeWindow();
                this.refreshShowFilamentSection();
            },
            immediate: false
        },
        'printInfo.uploadAndPrint': {
            async handler(newValue, oldValue) {
                // React to upload and print toggle changes
                if (!newValue) {
                    this.currentPage = 1; // Reset pagination when hiding filament section
                }
                this.resizeWindow();
                this.refreshShowFilamentSection();
            },
            immediate: false
        }

    }
};


const app = createApp(PrintSendApp)

// Register ElementPlus components globally
app.use(ElementPlus, {
    components: {
        ElInput,
        ElButton,
        ElPopover
    }
});

// Create and mount the Vue app
app.use(i18n);
app.mount('#app');

