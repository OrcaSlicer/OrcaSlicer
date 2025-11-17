# How to Contribute to the Wiki

This guide explains how to contribute to the OrcaSlicer wiki.

OrcaSlicer uses GitHub's wiki feature, which lets users and developers create and edit documentation collaboratively.

We encourage developers and users to contribute by updating existing pages and adding new content. This helps keep the documentation accurate and useful.

When adding new features, consider updating the wiki so users can access the latest guidance.

- [Wiki Structure](#wiki-structure)
  - [Home](#home)
    - [Index and Navigation](#index-and-navigation)
  - [File Naming and Organization](#file-naming-and-organization)
- [Orca to Wiki Redirection](#orca-to-wiki-redirection)
- [Formatting and Style](#formatting-and-style)
  - [Markdown Formatting](#markdown-formatting)
  - [Alerts and Callouts](#alerts-and-callouts)
- [Images](#images)
  - [Image Naming](#image-naming)
  - [Image Placement](#image-placement)
  - [Linking Images](#linking-images)
    - [Examples](#examples)
    - [Avoid the Following](#avoid-the-following)
    - [Resize Images](#resize-images)
  - [Image Cropping and Highlighting](#image-cropping-and-highlighting)
  - [Recommended Formats](#recommended-formats)
- [Structuring Content](#structuring-content)
- [Commands and Code Blocks](#commands-and-code-blocks)
- [External Links](#external-links)

## Wiki Structure

Each wiki page is a Markdown file located in the `doc` directory of the repository. The wiki is organized into sections that cover different areas of the project.

### Home

The Home page is the starting point for the OrcaSlicer wiki. From there you can navigate to sections and topics related to the project.

When you create a new page or section, link it from the Home page under the appropriate category.  
The Home page currently organizes content in these top-level entries:

- ![printer](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/resources/images/printer.svg?raw=true) [Printer Settings](home#printer-settings)
- ![filament](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/resources/images/filament.svg?raw=true) [Material Settings](home#material-settings)
- ![process](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/resources/images/process.svg?raw=true) [Process Settings](home#process-settings)
- ![tab_3d_active](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/resources/images/tab_3d_active.svg?raw=true) [Prepare](home#prepare)
- ![tab_calibration_active](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/resources/images/tab_calibration_active.svg?raw=true) [Calibrations](home#calibrations)
- ![im_code](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/resources/images/im_code.svg?raw=true) [Developer Section](home#developer-section)

Each section can have multiple pages covering specific topics. For example, the [Process Settings](home#process-settings) section includes pages on [quality](home#quality-settings), [support](home#support-settings), and [others](home#others-settings).

#### Index and Navigation

GitHub Wiki uses file names as page identifiers. To link to a page, use the file name without the `.md` extension. If a file lives in a subdirectory, **do not include the subdirectory** in the link; link directly to the file name from the Home page.

For example, if you add `doc/calibration/flow-rate-calib.md`, link it like this:

```markdown
[Flow Rate Calibration](flow-rate-calib)
```

For long pages, include a table of contents at the top to help readers find sections quickly.

```markdown
- [Wiki Structure](#wiki-structure)
  - [Home](#home)
    - [Index and Navigation](#index-and-navigation)
  - [File Naming and Organization](#file-naming-and-organization)
- [Formatting and Style](#formatting-and-style)
```

> [!NOTE]
> If you're adding a new section, follow the existing structure and make sure it doesn't already fit an existing category. Link it from the Home page accordingly.

### File Naming and Organization

When creating new pages, follow these file-naming conventions:

- Use unique file names to avoid conflicts.
- Use descriptive names that reflect the page's content.
- Use kebab-case for filenames (e.g.: `How-to-wiki.md`).
- If a page belongs to a section, include a suffix that clarifies it (for example, calibration pages should end with `-calib.md`, e.g. `flow-rate-calib.md`).
- Place files in the appropriate subdirectory when applicable (e.g.: `doc/calibration/` for calibration-related content).

## Orca to Wiki Redirection

OrcaSlicer can redirect users from the GUI to the appropriate wiki pages, making it easier to find relevant documentation.

The option-to-wiki mapping is defined in [src/slic3r/GUI/Tab.cpp](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/src/slic3r/GUI/Tab.cpp). Any option added with `append_single_option_line` can be mapped to a wiki page using a second string argument.

```cpp
optgroup->append_single_option_line("OPTION_NAME"); // Option without wiki page/redirection
optgroup->append_single_option_line("OPTION_NAME", "WIKI_PAGE"); // Option with wiki page and redirection
```

You can also point to a specific section within a wiki page by appending a fragment identifier (for example `#section-name`).

Example:

```cpp
optgroup->append_single_option_line("seam_gap","quality_settings_seam"); // Wiki page and redirection
optgroup->append_single_option_line("seam_slope_type", "quality_settings_seam#scarf-joint-seam"); // Wiki page and redirection to `Scarf Joint Seam` section
```

## Formatting and Style

Follow these style and formatting conventions when contributing to the wiki.

### Markdown Formatting

The wiki uses standard Markdown syntax for formatting and aims to maintain a consistent style across all pages. Avoid using raw HTML tags and prefer Markdown formatting instead.

Ensure your indentation is consistent, especially for code blocks and lists.

Refer to the [GitHub Markdown Guide](https://guides.github.com/features/mastering-markdown/) for more information on Markdown syntax.

### Alerts and Callouts

Use GitHub's alert syntax to add inline notes and warnings:

```markdown
> [!NOTE]
> Useful information that readers should know.

> [!TIP]
> Helpful advice for doing things more easily.

> [!IMPORTANT]
> Key information required to achieve a goal.

> [!WARNING]
> Urgent information to avoid problems.

> [!CAUTION]
> Warnings about risks or negative outcomes.
```

> [!NOTE]
> Refer to the [GitHub Alerts documentation](https://docs.github.com/get-started/writing-on-github/getting-started-with-writing-and-formatting-on-github/basic-writing-and-formatting-syntax#alerts) for more details.

## Images

Images are encouraged to enhance the clarity and quality of the wiki content. They help illustrate concepts, provide examples, and improve readability.

> [!CAUTION]
> Do not use images from third-party sources unless you have the proper permissions.

### Image Naming

- Use clear, descriptive filenames that reflect the image content.
- For section-specific images, include the section name or initials (for example `pa-[description].png` for Pressure Advance images).

### Image Placement

- General images should be placed in the `doc/images/` directory.
- Section-specific images should be stored in their corresponding subdirectories (e.g., `doc/images/calibration/` for calibration content).

> [!TIP]
> You can use `\resources\images` images used in the GUI.

### Linking Images

Always use raw GitHub URLs for image links to ensure correct display:

Format = `![`filename`](` + Base URL + filename.extension + Raw tag + `)`

- Base URL:

  ```markdown
  https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/
  ```

- Raw tag:

  ```markdown
  ?raw=true
  ```

#### Examples

- For an image in `doc/images/` named `calibration.png`:

  ```markdown
  ![calibration](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/calibration.png?raw=true)
  ```

- For an image in a subdirectory like `doc/images/GUI/combobox.png`:

  ```markdown
  ![combobox](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/GUI/combobox.png?raw=true)
  ```

> [!IMPORTANT]
> New or moved images may not appear in previews until the pull request is merged. Double-check paths and update links if you move files.

#### Avoid the Following

- Relative paths
- GitHub Assets/user-content/user-images URLs
- External image links from temporary or unreliable hosts
- Images containing personal or sensitive information
- Using images for content that can be expressed in text, such as equations or code—use Markdown syntax or Mermaid/Math formatting instead.

> [!NOTE]
> When contributing section-specific images, follow the naming conventions and directory structure.

#### Resize Images

Avoid the resize of images and let the Wiki handle it automatically.

If resizing is necessary (e.g., for thumbnails), use the following syntax:

HTML Format = `<img alt="` + filename + `"` + `src="` + Base URL + filename.extension + Raw tag + size limit.
Example:

```html
<img alt="IS_damp_marlin_print_measure" src="https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/InputShaping/IS_damp_marlin_print_measure.jpg?raw=true" height="200">
```

### Image Cropping and Highlighting

To ensure clarity:

- Crop images to focus on relevant areas.
- Use simple annotations (arrows, circles, rectangles) to highlight important parts without overloading the image.

### Recommended Formats

- **JPG:** Suitable for photographs. Avoid for images with text or fine detail due to compression artifacts.
- **PNG:** Ideal for screenshots or images with transparency. Ensure sufficient contrast for light and dark modes.
- **SVG:** Preferred when possible. SVGs support theme adaptation (light/dark mode), making them ideal for icons and diagrams.

## Structuring Content

Each page should have a clear objective. After a short introduction, choose a structure that fits the content:

- **Step-by-step guides:** Use for sequential procedures (for example calibration).
- **GUI-based reference:** Describe settings following OrcaSlicer's UI when sequence isn't required.

Example: explain **Layer Height** before **Initial Layer Height**, since the former is global and the latter only applies to the first layer.

## Commands and Code Blocks

When adding commands or code blocks please use the [Code Block with Syntax Highlighting feature of Markdown](https://docs.github.com/en/get-started/writing-on-github/working-with-advanced-formatting/creating-and-highlighting-code-blocks#syntax-highlighting).

- Use triple backticks (```) to enclose code blocks.
- Specify the language for proper highlighting and readability.

````markdown
```json
{
  "key": "value"
}
```
````

```json
{
  "key": "value"
}
```

## External Links

Be careful when linking to external resources.  
Ensure links are relevant and reliable and cite papers or articles when appropriate.
