1) GetOnlinePrinter

路由:

- GET /api/get-online-printer/

请求格式:

- 不是 JSON body
- 读取的是 `c.Request().FormValue("username")` 和 `c.Request().FormValue("password")`
- 所以通常是 query 参数或 form 表单参数

```ts
type GetOnlinePrinterRequest = {
  username: string;
  password: string;
};

// 也可写成 query 形式:
// GET /api/get-online-printer/?username=alice&password=123456
```

响应格式:

- 成功：`{ status: true, message: "OK", data: [...] }`
- 失败：`{ status: false, message: string, data: {} }`

```ts
type PrinterInfo = {
  printer_id: string;
  model: string;
  name: string;
  id: number;
  group: number;
};

type GetOnlinePrinterSuccessResponse = {
  status: true;
  message: "OK";
  data: PrinterInfo[];
};

type GetOnlinePrinterErrorResponse = {
  status: false;
  message: string;
  data: {};
};

type GetOnlinePrinterResponse =
  | GetOnlinePrinterSuccessResponse
  | GetOnlinePrinterErrorResponse;
```

例子:

```ts
const res: GetOnlinePrinterSuccessResponse = {
  status: true,
  message: "OK",
  data: [
    {
      printer_id: "P-001",
      model: "Creality CR-10",
      name: "Printer A",
      id: 1001,
      group: 1
    }
  ]
};
```

2) ShareFile

路由:

- POST /api/share-file/

请求格式:

- 这是 multipart/form-data，不是 JSON
- 关键字段来自：
  - `username`
  - `password`
  - `filename`
  - `printers`
  - `file`
- `file` 是上传文件对象
- `printers` 是数组/重复字段，代码里用 `Form["printers"]`

```ts
type ShareFileRequest = {
  username?: string;      // 若无 cookie/auth，可传
  password?: string;      // 若无 cookie/auth，可传
  filename?: string;      // 可选，默认 "Untitled.gcode"
  printers?: string[];    // 目标打印机 ID 列表
  file: File;             // 必填，multipart/form-data 文件
};
```

示例：

```ts
const formData = new FormData();
formData.append("username", "alice");
formData.append("password", "123456");
formData.append("filename", "demo.gcode");
formData.append("printers", "printer-001");
formData.append("printers", "printer-002");
formData.append("file", file);

await fetch("/api/share-file/", {
  method: "POST",
  body: formData
});
```

响应格式:

- 成功：

```ts
type ShareFileSuccessData = {
  filename: string;
  file_size: number;
  file_img: string;
  print_duration: string;
  material_weight: string;
};

type ShareFileSuccessResponse = {
  status: true;
  message: "File shared successfully";
  data: ShareFileSuccessData;
};
```

- 失败：

```ts
type ShareFileErrorResponse = {
  status: false;
  message: string;
  data: {};
};
```

总类型：

```ts
type ShareFileResponse =
  | ShareFileSuccessResponse
  | ShareFileErrorResponse;
```

示例成功响应：

```ts
const response: ShareFileSuccessResponse = {
  status: true,
  message: "File shared successfully",
  data: {
    filename: "demo_1234567890.gcode",
    file_size: 245678,
    file_img: "demo_1234567890.png",
    print_duration: "01:30:00",
    material_weight: "120.5"
  }
};
```

补充说明:

- `GetOnlinePrinter` 的请求不是 JSON body，而是 form/query 参数
- `ShareFile` 是 multipart/form-data, `file` 必须作为上传文件处理
- 两个接口的错误返回统一是：

```ts
{ status: false, message: string, data: {} }
```
