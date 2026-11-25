# sniplite upload server (Cloudflare Worker + R2)

Nút **Tải lên** (Ctrl+U) trong sniplite gửi ảnh tới server này. Server trả về link ảnh, sniplite chép link vào clipboard và ghi vào `uploads.log` (nằm cạnh `sniplite.ini`).

## Triển khai

```powershell
cd G:\PROJECT_PRIVATE\sniplite\server
npx wrangler login                     # đăng nhập Cloudflare (mở trình duyệt)
npx wrangler r2 bucket create sniplite # tạo bucket R2
npx wrangler secret put TOKEN          # nhập một chuỗi ngẫu nhiên dài, dùng làm upload_token
npx wrangler deploy                    # in ra https://sniplite-upload.<subdomain>.workers.dev
```

## Cấu hình sniplite

Thêm vào `sniplite.ini` (cạnh `sniplite.exe`):

```ini
[sniplite]
upload_url=https://sniplite-upload.<subdomain>.workers.dev
upload_token=<TOKEN ở trên>
```

## API

| Method | Path | Auth | Kết quả |
|---|---|---|---|
| POST | `/upload` | Bearer | `{"id","url"}`, body là PNG/JPEG, tối đa 10 MB |
| GET | `/list` | Bearer | danh sách ảnh, mới nhất trước |
| DELETE | `/<id>` | Bearer | xóa ảnh |
| GET | `/<id>` | công khai | file ảnh |
| GET | `/` | nhập token trên trang | trang quản lý: xem, chép link, xóa |

Ai có link chỉ xem được đúng ảnh đó. Tên ảnh là 10 ký tự ngẫu nhiên.
