// Printer status utility functions
// Shared between printer_manager and print_send

const PrinterStatusUtils = {
    /**
     * Check if progress text should be shown
     * @param {number} printerStatus - Printer status code
     * @param {number} connectStatus - Connection status (0=offline, 1=online)
     * @returns {boolean}
     */
    canShowProgressText(printerStatus, connectStatus) {
        return printerStatus === 1 || printerStatus === 2 || printerStatus === 3;
    },

    /**
     * printer is offline if connectStatus is 0 or printerStatus is -1
     * @param {number} printerStatus - Printer status code
     * @param {number} connectStatus - Connection status (0=offline, 1=online)
     */
    isPrinterOffline(printerStatus, connectStatus) {
        return connectStatus === 0 || printerStatus === -1;
    },

    /**
     * Get printer status text
     * @param {number} printerStatus - Printer status code
     * @param {number} connectStatus - Connection status
     * @param {Function} $t - i18n translation function
     * @returns {string}
     */
    getPrinterStatus(printerStatus, connectStatus, $t) {
        // If not connected, always show Offline
        if (connectStatus === 0) {
            return $t("printerManager.offline");
        }

        const statusMap = {
            [-1]: $t("printerManager.offline"),
            [0]: $t("printerManager.idle"),
            [1]: $t("printerManager.printing"),
            [2]: $t("printerManager.paused"),
            [3]: $t("printerManager.pausing"),
            [4]: $t("printerManager.canceled"),
            [5]: $t("printerManager.selfChecking"),
            [6]: $t("printerManager.autoLeveling"),
            [7]: $t("printerManager.pidCalibrating"),
            [8]: $t("printerManager.resonanceTesting"),
            [9]: $t("printerManager.updating"),
            [10]: $t("printerManager.fileCopying"),
            [11]: $t("printerManager.fileTransferring"),
            [12]: $t("printerManager.homing"),
            [13]: $t("printerManager.preheating"),
            [14]: $t("printerManager.filamentOperating"),
            [15]: $t("printerManager.extruderOperating"),
            [16]: $t("printerManager.printCompleted"),
            [17]: $t("printerManager.rfidRecognizing"),
            [18]: $t("printerManager.videoComposing"),
            [19]: $t("printerManager.emergencyStop"),
            [20]: $t("printerManager.powerLossRecovery"),
            [21]: $t("printerManager.initializing"),
            [998]: $t("printerManager.busy"),
            [999]: $t("printerManager.error"),
            [1000]: $t("printerManager.idNotMatch"),
            [1001]: $t("printerManager.authError"),
            [10000]: $t("printerManager.unknown")
        };

        return statusMap[printerStatus] || "";
    },

    /**
     * Get printer status style (color and background)
     * @param {number} printerStatus - Printer status code
     * @param {number} connectStatus - Connection status
     * @returns {Object} Style object with color and backgroundColor
     */
    getPrinterStatusStyle(printerStatus, connectStatus) {
        if (connectStatus === 0 || printerStatus === -1) {
            return {
                color: 'var(--printer-status-offline-color)',
                backgroundColor: 'var(--printer-status-offline-bg)'
            };
        }
        if (printerStatus === 16 || printerStatus === 0) {
            return {
                color: 'var(--printer-status-success-color)',
                backgroundColor: 'var(--printer-status-success-bg)'
            };
        } else if (printerStatus === 999 || printerStatus === 998) {
            return {
                color: 'var(--printer-status-error-color)',
                backgroundColor: 'var(--printer-status-error-bg)'
            };
        } else if (printerStatus === 1000 || printerStatus === 2 || printerStatus === 3 || printerStatus === 20) {
            return {
                color: 'var(--printer-status-warning-color)',
                backgroundColor: 'var(--printer-status-warning-bg)'
            };
        } else {
            return {
                color: 'var(--printer-status-primary-color)',
                backgroundColor: 'var(--printer-status-primary-bg)'
            };
        }
    },

    /**
     * Check if warning icon should be shown
     * @param {number} printerStatus - Printer status code
     * @param {number} connectStatus - Connection status
     * @returns {boolean}
     */
    shouldShowWarningIcon(printerStatus, connectStatus) {
        return connectStatus === 0 && (printerStatus === 1000 || printerStatus === 1001);
    },

    /**
     * Get warning tooltip text
     * @param {number} printerStatus - Printer status code
     * @param {Function} $t - i18n translation function
     * @returns {string}
     */
    getWarningTooltip(printerStatus, $t) {
        if (printerStatus === 1000) {
            return $t("printerManager.idNotMatch");
        } else if (printerStatus === 1001) {
            return $t("printerManager.authError");
        }
        return "";
    },

    /**
     * Get printer progress percentage
     * @param {Object} printer - Printer object with printTask
     * @returns {number} Progress percentage (0-100)
     */
    getPrinterProgress(printer) {
        return (printer.printTask && typeof printer.printTask.progress === 'number') ? printer.printTask.progress : 0;
    },

    /**
     * Get printer remaining time formatted as HH:MM:SS
     * @param {Object} printer - Printer object with printTask
     * @returns {string} Formatted time string
     */
    getPrinterRemainingTime(printer) {
        if (!printer.printTask) return '';

        const { currentTime, totalTime, estimatedTime } = printer.printTask;

        // If estimatedTime is provided, it already represents remaining seconds
        const remainingTime = (typeof estimatedTime === 'number' && estimatedTime > 0)
            ? estimatedTime
            : Math.max(0, (totalTime || 0) - (currentTime || 0));
        if (remainingTime === 0) return '--:--:--';    
        const hours = Math.floor(remainingTime / 3600);
        const minutes = Math.floor((remainingTime % 3600) / 60);
        const seconds = remainingTime % 60;
        const pad = (n) => String(n).padStart(2, '0');

        return `${pad(hours)}:${pad(minutes)}:${pad(seconds)}`;
    }
};
