# Server upload cho sniplite (Cloudflare Worker + R2)

Hướng dẫn này giúp bạn tự dựng server nhận ảnh cho nút **Tải lên** (Ctrl+U) của sniplite. Server chạy trên tài khoản Cloudflare của chính bạn. Ảnh lưu trong R2, bạn xem lại và xóa được.

## Cách hoạt động

```
sniplite ──POST /upload (PNG, Bearer token)──► Worker ──► R2 bucket "sniplite"
   ▲                                              │
   └────────── {"id": "...", "url": "..."} ◄──────┘
```

- sniplite chép link vào clipboard, hiện cửa sổ **Mở / Chép / Google / Bing / Yandex**, và ghi link vào `uploads.log` (nằm cạnh `sniplite.ini`).
- Ai có link chỉ xem được đúng ảnh đó. Tên ảnh là 10 ký tự ngẫu nhiên.
- Muốn upload, liệt kê hay xóa ảnh thì phải có token.

## Cần chuẩn bị

1. Một tài khoản Cloudflare (gói Free là đủ): https://dash.cloudflare.com/sign-up
2. **Bật R2** trong dashboard: menu **R2 Object Storage**, rồi làm theo hướng dẫn kích hoạt. Cloudflare có thể yêu cầu thêm thẻ hoặc PayPal, kể cả khi bạn chỉ dùng phần miễn phí.
3. Node.js 18 trở lên (để chạy `npx wrangler`): https://nodejs.org

## Triển khai (làm một lần)

Mở PowerShell trong thư mục `server`:

```powershell
cd <thư mục sniplite>\server

# 1. Đăng nhập Cloudflare (mở trình duyệt)
npx wrangler login

# 2. Tạo bucket R2 tên "sniplite" (đúng tên trong wrangler.toml)
npx wrangler r2 bucket create sniplite

# 3. Tạo token bí mật: một chuỗi ngẫu nhiên dài, ví dụ:
-join ((48..57)+(65..90)+(97..122) | Get-Random -Count 40 | % {[char]$_})
#    rồi lưu vào Worker (dán chuỗi đó khi được hỏi):
npx wrangler secret put TOKEN

# 4. Deploy
npx wrangler deploy
```

Lần deploy đầu tiên, wrangler sẽ hỏi tên **workers.dev subdomain** cho tài khoản:
- Hãy gõ một tên riêng, chỉ gồm chữ thường, số và dấu `-`, ví dụ `ten-cua-ban-snip`. Đừng gõ `y` hay để trống.
- Tên này nằm trong mọi link ảnh, nên đừng dùng tên thật hay email.

Deploy xong, wrangler in ra địa chỉ dạng:

```
https://sniplite-upload.<subdomain>.workers.dev
```

Subdomain mới có thể cần vài phút mới truy cập được. Trước đó, HTTPS có thể báo lỗi TLS (`SEC_E_ILLEGAL_MESSAGE`).

## Cấu hình sniplite

Mở `sniplite.ini` nằm cạnh `sniplite.exe`:
- Bản cài: `%LOCALAPPDATA%\Programs\sniplite\sniplite.ini`
- Bản tự build: `build\sniplite.ini`

Thay hai giá trị mẫu bằng địa chỉ và token của bạn:

```ini
[sniplite]
upload_url=https://sniplite-upload.<subdomain>.workers.dev
upload_token=<token ở bước 3>
```

- Không cần khởi động lại sniplite, vì mỗi lần upload app đều đọc lại hai dòng này.
- Khi `upload_url` vẫn là `YOUR_URL` hoặc không phải địa chỉ `http(s)://`, nút Tải lên chỉ hiện hướng dẫn cấu hình.

## Dùng

| Việc | Cách |
|---|---|
| Tải ảnh lên | F1, chọn vùng, rồi bấm nút mũi tên lên hoặc **Ctrl+U** |
| Mở hoặc chép link | Nút **Mở** / **Chép** trong cửa sổ hiện ra (link cũng đã tự được chép) |
| Tìm ảnh tương tự | Nút **Google** / **Bing** / **Yandex** trong cùng cửa sổ |
| Xem và xóa ảnh đã tải | Chuột phải icon ở khay, chọn **Ảnh đã tải lên**, rồi nhập token trên trang |
| Lịch sử trên máy | `uploads.log` cạnh `sniplite.ini` |

Khi tìm ảnh tương tự, Google, Bing hoặc Yandex sẽ tự tải ảnh từ link của bạn về để tìm.

## Kiểm tra server bằng curl

```powershell
$U = "https://sniplite-upload.<subdomain>.workers.dev"
$T = "<token>"
curl.exe -s -X POST -H "Authorization: Bearer $T" -H "Content-Type: image/png" --data-binary "@anh.png" "$U/upload"
curl.exe -s -H "Authorization: Bearer $T" "$U/list"
curl.exe -s -X DELETE -H "Authorization: Bearer $T" "$U/<id>"
```

## API

| Method | Path | Cần token | Kết quả |
|---|---|---|---|
| POST | `/upload` | có | `{"id","url"}`. Body là PNG hoặc JPEG, tối đa 10 MB |
| GET | `/list` | có | danh sách ảnh `[{id,size,uploaded}]`, mới nhất trước |
| DELETE | `/<id>` | có | 204 |
| GET | `/<id>` | không | file ảnh, có cache lâu dài |
| GET | `/` | nhập trên trang | trang quản lý: xem, chép link, xóa |

## Chi phí

Gói miễn phí (theo tài liệu Cloudflare, hãy kiểm tra lại ở https://developers.cloudflare.com/r2/pricing):
- R2: 10 GB lưu trữ, 1 triệu lượt ghi và 10 triệu lượt đọc mỗi tháng. Xóa và băng thông tải về miễn phí.
- Workers: 100.000 request mỗi ngày.

Dùng cá nhân thường nằm trong hạn mức. Bạn nên đặt **Billing notification** trong dashboard để được báo nếu phát sinh phí.

## Cập nhật hoặc đổi token

```powershell
npx wrangler secret put TOKEN   # đổi token, rồi sửa upload_token trong sniplite.ini
npx wrangler deploy             # sau khi sửa worker.js
```

## Lỗi thường gặp

| Hiện tượng | Nguyên nhân / cách sửa |
|---|---|
| `Tải lên thất bại: HTTP 401` | Token trong `sniplite.ini` khác token đã lưu bằng `wrangler secret put TOKEN` |
| `HTTP 413` / `HTTP 415` | Ảnh lớn hơn 10 MB, hoặc không phải PNG/JPEG |
| `... (12xxx)` hoặc lỗi kết nối | Sai `upload_url`, mất mạng, hoặc subdomain mới chưa sẵn sàng (đợi vài phút) |
| `Subdomain is unavailable` khi deploy | Tên đã có người dùng: chọn tên khác |
| `r2 bucket create` báo lỗi | Chưa bật R2 trong dashboard |
| Mất `upload_url` sau khi đổi cài đặt | Không còn xảy ra từ bản này: app giữ hai dòng upload khi ghi lại `sniplite.ini` |
