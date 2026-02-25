//--------------------------------------------------------------------------------
/**
\file     Common.cpp
\brief    Common function implementation

\version  v1.0.2505.9191
\date     2025-05-19

<p>Copyright (c) 2017-2025</p>
*/
//----------------------------------------------------------------------------------

#include "Common.h"

//----------------------------------------------------------------------------------
/**
\Get enum from device and insert UI ComboBox items
\param[in]      m_hDevice       Device handle
\param[in]      qobjComboBox    ComboBox object
\param[in]      emFeatureID     GxIAPI Featrue ID
\param[out]
\return GX_STATUS
*/
//----------------------------------------------------------------------------------
GX_STATUS InitComboBox(GX_DEV_HANDLE m_hDevice, QComboBox *qobjComboBox, char *strFeatureID)
{
    GX_STATUS   emStatus = GX_STATUS_SUCCESS;
    uint32_t    ui32EntryNums = 0;
    int64_t     i64CurrentValue = 0;

    // Get entry quantity firstly
    GX_ENUM_VALUE stEnumValue;
    emStatus = GXGetEnumValue(m_hDevice, strFeatureID, &stEnumValue);
    ui32EntryNums = stEnumValue.nSupportedNum;
    if (emStatus != GX_STATUS_SUCCESS)
    {
        return emStatus;
    }


    // Get current setting
    i64CurrentValue = stEnumValue.stCurValue.nCurValue;

#if QT_VERSION < QT_VERSION_CHECK(5,0,0)
    for (uint32_t i = 0; i < ui32EntryNums; i++)
    {
        qobjComboBox->insertItem(ui32EntryNums, stEnumValue.nArrySupportedValue[i].strCurSymbolic, qVariantFromValue(stEnumValue.nArrySupportedValue[i].nCurValue));
    }
    // Set current index
    qobjComboBox->setCurrentIndex(qobjComboBox->findData(qVariantFromValue(i64CurrentValue)));
#else
    for (uint32_t i = 0; i < ui32EntryNums; i++)
    {
        qobjComboBox->insertItem(ui32EntryNums, stEnumValue.nArrySupportedValue[i].strCurSymbolic, QVariant::fromValue(stEnumValue.nArrySupportedValue[i].nCurValue));
    }
    // Set current index
    qobjComboBox->setCurrentIndex(qobjComboBox->findData(QVariant::fromValue(i64CurrentValue)));
#endif
    return GX_STATUS_SUCCESS;
}

//----------------------------------------------------------------------------------
/**
\ Show Error Message
\param[in]  error_status     error status code input
\param[out]
\return  void
*/
//----------------------------------------------------------------------------------
void ShowErrorString(GX_STATUS error_status)
{
    char *error_info = NULL;
    size_t    size        = 0;
    GX_STATUS emStatus     = GX_STATUS_ERROR;

    // Get the length of the error message and alloc memory for error info
    emStatus = GXGetLastError(&error_status, NULL, &size);

    // Alloc memory for error info
    try
    {
        error_info = new char[size];
    }
    catch (std::bad_alloc& e)
    {
        QMessageBox::about(NULL, "Error", "Alloc error info Faild!");
        return;
    }

    // Get the error message and display
    emStatus = GXGetLastError (&error_status, error_info, &size);

    if (emStatus != GX_STATUS_SUCCESS)
    {
        QMessageBox::about(NULL, "Error", "  Interface of GXGetLastError call failed! ");
    }
    else
    {
        QMessageBox::about(NULL, "Error", QString("%1").arg(QString(QLatin1String(error_info))));
    }

    // Release memory alloced
    if (NULL != error_info)
    {
        delete[] error_info;
        error_info = NULL;
    }

    return;
}
