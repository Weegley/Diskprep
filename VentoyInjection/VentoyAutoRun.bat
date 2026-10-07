@echo off
setlocal

for %%D in (C D E F G H I J K L M N O P Q R S T U V W Y Z) do (
    if exist "%%D:\$WinPEDriver$\" (
        echo [DRIVERS] Found %%D:\$WinPEDriver$
        pnputil /add-driver "%%D:\$WinPEDriver$\*.inf" /subdirs /install
    )
)

if exist "X:\Drivers\" (
    echo [DRIVERS] Found X:\Drivers
    pnputil /add-driver "X:\Drivers\*.inf" /subdirs /install
)


pnputil /scan-devices