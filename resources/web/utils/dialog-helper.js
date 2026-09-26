/**
 * Dialog Helper - Unified dialog style utility
 * Provides consistent dialog appearance and experience for the entire project
 */

const DialogHelper = {
    /**
     * Default dialog configuration
     */
    defaultOptions: {
        center: true,
        showClose: true,
        dangerouslyUseHTMLString: true,
        customStyle: {
            minWidth:"600px"
        },
        customClass: 'custom-dialog-helper',
    },

    /**
     * Create HTML content template
     * @param {string} message - Message content
     * @param {string} imageSrc - Image path (optional)
     * @param {number} imageSize - Image size (default 52)
     * @returns {string} HTML string
     */
    createContent(message, imageSrc = null, imageSize = 52) {
        let html = '<div style="text-align: center; padding: 20px 16px;">';
        
        if (imageSrc) {
            html += `
                <div style="margin-bottom: 20px;">
                    <img src="${imageSrc}" alt="Dialog Icon" width="${imageSize}" height="${imageSize}" />
                </div>
            `;
        }
        
        html += `
            <div style="font-size: 16px; color: #606266; line-height: 1.6;">
                ${message}
            </div>
        </div>`;
        
        return html;
    },

    /**
     * Show confirmation dialog
     * @param {Object} options - Configuration options
     * @param {string} options.title - Title
     * @param {string} options.message - Message content
     * @param {string} options.confirmText - Confirm button text
     * @param {string} options.cancelText - Cancel button text
     * @param {boolean} options.showCancelButton - Whether to show cancel button (optional, default true)
     * @param {string} options.imageSrc - Image path (optional)
     * @param {number} options.imageSize - Image size (optional, default 52)
     * @param {Object} options.extraOptions - Additional ElementPlus configuration (optional)
     * @returns {Promise}
     */
    confirm({
        title,
        message,
        confirmText,
        cancelText,
        showCancelButton = true,
        imageSrc = null,
        imageSize = 52,
        extraOptions = {}
    }) {
        const content = this.createContent(message, imageSrc, imageSize);
        
        const options = {
            ...this.defaultOptions,
            confirmButtonText: confirmText,
            showCancelButton: showCancelButton,
            ...extraOptions
        };

        // Only set cancelButtonText if showCancelButton is true
        if (showCancelButton && cancelText) {
            options.cancelButtonText = cancelText;
        }

        return ElementPlus.ElMessageBox.confirm(content, title, options);
    },

};

// Global exposure
window.DialogHelper = DialogHelper;
