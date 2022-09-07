package ru.yandex.inside.yt.kosher.ytree;

/**
 * @author sankear
 */
public interface YTreeIntegerNode extends YTreeScalarNode<Long> {
    boolean isSigned();

    long getLong();

    /**
     * Set signed long value.
     */
    long setLong(long value);

    /**
     * Set unsigned long value.
     */
    long setUnsignedLong(long value);

    default int getInt() {
        return (int) getLong();
    }
}
